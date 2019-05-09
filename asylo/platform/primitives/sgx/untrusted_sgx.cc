/*
 *
 * Copyright 2019 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "asylo/platform/primitives/sgx/untrusted_sgx.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstdlib>

#include "asylo/platform/primitives/sgx/sgx_error_space.h"
#include "asylo/platform/primitives/untrusted_primitives.h"
#include "asylo/util/elf_reader.h"
#include "asylo/util/file_mapping.h"
#include "asylo/util/status.h"
#include "asylo/util/status_macros.h"
#include "asylo/util/statusor.h"
#include "include/sgx_edger8r.h"
#include "include/sgx_urts.h"

// Edger8r-generated ocall table.
struct ocall_table_t {
    size_t nr_ocall;
    void *table[];
};

// This global is written into at compile time by the untrusted bridge files
// generated by edger8r.
extern "C" ABSL_CONST_INIT const ocall_table_t ocall_table_bridge;

namespace asylo {
namespace primitives {

namespace {

constexpr absl::string_view kCallingProcessBinaryFile = "/proc/self/exe";

constexpr int kMaxEnclaveCreateAttempts = 5;

// Edger8r-generated primitives ecall marshalling struct.
struct ms_ecall_dispatch_trusted_call_t {
  // Return value from the trusted call.
  int ms_retval;

  // Trusted selector value.
  uint64_t ms_selector;

  // Pointer to the parameter stack passed to primitives::EnclaveCall. The
  // pointer is interpreted as a void pointer as edger8r only allows trivial
  // data types to be passed across the bridge.
  void* ms_buffer;
};

}  // namespace

SgxEnclaveClient::~SgxEnclaveClient() = default;

StatusOr<std::shared_ptr<Client>> SgxBackend::Load(
    const absl::string_view enclave_name, void *base_address,
    absl::string_view enclave_path, size_t enclave_size,
    const EnclaveConfig &config, bool debug,
    std::unique_ptr<Client::ExitCallProvider> exit_call_provider) {
  std::shared_ptr<SgxEnclaveClient> client(
      new SgxEnclaveClient(enclave_name, std::move(exit_call_provider)));
  client->base_address_ = base_address;

  int updated;
  sgx_status_t status;
  const uint32_t ex_features = SGX_CREATE_ENCLAVE_EX_ASYLO;
  asylo_sgx_config_t create_config = {
    .base_address = &client->base_address_,
    .enclave_size = enclave_size,
    .enable_user_utility = config.enable_fork()
  };
  const void* ex_features_p[32] = { nullptr };
  ex_features_p[SGX_CREATE_ENCLAVE_EX_ASYLO_BIT_IDX] = &create_config;
  for (int i = 0; i < kMaxEnclaveCreateAttempts; ++i) {
    status = sgx_create_enclave_ex(
        std::string(enclave_path).c_str(), debug, &client->token_, &updated,
        &client->id_, /*misc_attr=*/nullptr, ex_features, ex_features_p);

    LOG_IF(WARNING, status != SGX_SUCCESS)
        << "Failed to create an enclave, attempt=" << i
        << ", status=" << status;
    if (status != SGX_INTERNAL_ERROR_ENCLAVE_CREATE_INTERRUPTED) {
      break;
    }
  }

  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to create an enclave");
  }

  client->size_ = sgx_enclave_size(client->id_);
  return client;
}

StatusOr<std::shared_ptr<Client>> SgxEmbeddedBackend::Load(
    const absl::string_view enclave_name, void *base_address,
    absl::string_view section_name, size_t enclave_size,
    const EnclaveConfig &config, bool debug,
    std::unique_ptr<Client::ExitCallProvider> exit_call_provider) {
  std::shared_ptr<SgxEnclaveClient> client(
      new SgxEnclaveClient(enclave_name, std::move(exit_call_provider)));
  client->base_address_ = base_address;

  // If an address is specified to load the enclave, temporarily reserve it to
  // prevent these mappings from occupying that location.
  if (base_address && enclave_size > 0 &&
      mmap(base_address, enclave_size, PROT_NONE, MAP_SHARED | MAP_ANONYMOUS,
           -1, 0) != base_address) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to reserve enclave memory");
  }

  FileMapping self_binary_mapping;
  ASYLO_ASSIGN_OR_RETURN(self_binary_mapping, FileMapping::CreateFromFile(
                                                  kCallingProcessBinaryFile));

  ElfReader self_binary_reader;
  ASYLO_ASSIGN_OR_RETURN(self_binary_reader, ElfReader::CreateFromSpan(
                                                 self_binary_mapping.buffer()));

  absl::Span<const uint8_t> enclave_buffer;
  ASYLO_ASSIGN_OR_RETURN(enclave_buffer, self_binary_reader.GetSectionData(
                                             std::string(section_name)));

  if (base_address && enclave_size > 0 &&
      munmap(base_address, enclave_size) < 0) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to release enclave memory");
  }

  sgx_status_t status;
  const uint32_t ex_features = SGX_CREATE_ENCLAVE_EX_ASYLO;
  asylo_sgx_config_t create_config = {
    .base_address = &client->base_address_,
    .enclave_size = enclave_size,
    .enable_user_utility = config.enable_fork()
  };
  const void* ex_features_p[32] = { nullptr };
  ex_features_p[SGX_CREATE_ENCLAVE_EX_ASYLO_BIT_IDX] = &create_config;
  for (int i = 0; i < kMaxEnclaveCreateAttempts; ++i) {
    status = sgx_create_enclave_from_buffer_ex(
        const_cast<uint8_t *>(enclave_buffer.data()), enclave_buffer.size(),
        debug, &client->id_, /*misc_attr=*/nullptr, ex_features,
        ex_features_p);

    if (status != SGX_INTERNAL_ERROR_ENCLAVE_CREATE_INTERRUPTED) {
      break;
    }
  }

  client->size_ = sgx_enclave_size(client->id_);

  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to create an enclave");
  }

  return client;
}

Status SgxEnclaveClient::Initialize(
    const char *enclave_name, const char *input, size_t input_len,
    char **output, size_t *output_len) {
  UntrustedParameterStack params;
  params.PushByReference(Extent{enclave_name});
  params.PushByReference(Extent{input, input_len});
  params.PushByReference(Extent{output});
  params.PushByReference(Extent{output_len});
  return EnclaveCall(kSelectorAsyloInit, &params);
}

Status SgxEnclaveClient::Destroy() {
  sgx_status_t status = sgx_destroy_enclave(id_);
  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to destroy enclave");
  }
  return Status::OkStatus();
}

sgx_enclave_id_t SgxEnclaveClient::GetEnclaveId() const { return id_; }

size_t SgxEnclaveClient::GetEnclaveSize() const { return size_; }

void *SgxEnclaveClient::GetBaseAddress() const { return base_address_; }

void SgxEnclaveClient::GetLaunchToken(sgx_launch_token_t *token) const {
  memcpy(token, &token_, sizeof(token_));
}

bool SgxEnclaveClient::IsClosed() const {
  abort();
}

Status SgxEnclaveClient::EnclaveCallInternal(uint64_t selector,
                                             UntrustedParameterStack *params) {
  ms_ecall_dispatch_trusted_call_t ms;
  ms.ms_selector = selector;
  ms.ms_buffer = reinterpret_cast<void *>(params);

  const ocall_table_t* table = &ocall_table_bridge;
  sgx_status_t status =
      sgx_ecall(id_, /*index=*/0, table, &ms, /*is_utility=*/false);

  if (status != SGX_SUCCESS) {
    // Return a Status object in the SGX error space.
    return Status(status, "Call to primitives ecall endpoint failed");
  }
  if (ms.ms_retval) {
    return Status(
        error::GoogleError::INTERNAL, "Enclave call failed inside enclave");
  }
  return Status::OkStatus();
}

}  // namespace primitives
}  // namespace asylo
