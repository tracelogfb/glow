/*
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
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
 */

#include "NNPIResource.h"
#include "nnpi_inference.h"
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef USE_AVX
#include <immintrin.h>
static inline void ConvertI64toI32(int64_t const *i64Data, int32_t *i32Data,
                                   uint32_t elements) {
  const __mmask8 masks[9] = {
      0b0, 0b1, 0b11, 0b111, 0b1111, 0b11111, 0b111111, 0b1111111, 0b11111111,
  };
  constexpr uint32_t vecSize = (sizeof(__m512i) / sizeof(int64_t));
  const uint32_t fullIterations = (elements / vecSize);
  const uint32_t tailElements = (elements % vecSize);

  for (uint32_t i = 0; i < fullIterations; i++) {
    __m512i i64vec = _mm512_maskz_loadu_epi64(masks[vecSize], i64Data);
    _mm512_mask_cvtepi64_storeu_epi32(i32Data, masks[vecSize], i64vec);
    i64Data += vecSize;
    i32Data += vecSize;
  }
  if (tailElements > 0) {
    __m512i i64vec = _mm512_maskz_loadu_epi64(masks[tailElements], i64Data);
    _mm512_mask_cvtepi64_storeu_epi32(i32Data, masks[tailElements], i64vec);
  }
}
#else  // USE_AVX
static inline void ConvertI64toI32(int64_t const *i64Data, int32_t *i32Data,
                                   uint32_t elements) {
  for (size_t i = 0; i < elements; i++) {
    i32Data[i] = static_cast<int32_t>(i64Data[i]);
  }
}
#endif // USE_AVX

static size_t CalcDescSize(const NNPIResourceDesc *desc) {
  if (desc->numDims == 0) {
    return 0;
  }

  size_t elements = 1;
  for (uint32_t d = 0; d < desc->numDims; d++) {
    elements *= desc->dims[d];
  }
  size_t elemSize = 0;
  switch (desc->dataType) {
  case NNPI_INF_PRECISION_FLOAT32:
  case NNPI_INF_PRECISION_INT32:
  case NNPI_INF_PRECISION_UINT32:
    elemSize = 4;
    break;
  case NNPI_INF_PRECISION_FLOAT16:
    elemSize = 2;
    break;
  case NNPI_INF_PRECISION_INT8:
  case NNPI_INF_PRECISION_UINT8:
    // case NNPI_INF_PRECISION_BOOLEAN://todo: uncomment once supported
    elemSize = 1;
    break;
  default:
    LOG_AND_RETURN_IF(ERROR, true, "Invalid precision", 0);
    break;
  }

  return elements * elemSize;
}

static std::string ConvertHexTextFormat(void *data, size_t size) {
  std::stringstream ss;

  uint8_t *buf = reinterpret_cast<uint8_t *>(data);

  for (size_t cl = 0; cl < size; cl += 64) {
    size_t size_to_read = std::min((size_t)64, size - cl);
    for (size_t i = 0; i < size_to_read; i++) {
      ss << std::setfill('0') << std::setw(2) << std::uppercase << std::hex
         << (uint32_t)(buf[cl + (size_to_read - 1 - i)]);
    }

    ss << "\n";
  }

  return ss.str();
}

static void DumpToFile(const std::string &filename, void *data, size_t size) {
  std::fstream fs(filename, std::ios::out | std::ios::binary);

  if (fs.is_open()) {
    auto res = ConvertHexTextFormat(data, size);
    fs << res;
    fs.close();
  } else {
    LOG(ERROR) << "cannot open the file \"" << filename << "\" for writing";
  }
}

namespace glow {
namespace runtime {

NNPIResource::NNPIResource() {
  adapter_ = NNPI_INVALID_NNPIHANDLE;
  device_ = NNPI_INVALID_NNPIHANDLE;
  memset(name_, 0, sizeof(name_));
  memset(&desc_, 0, sizeof(desc_));
  deviceResource_ = NNPI_INVALID_NNPIHANDLE;
  hostResource_ = NNPI_INVALID_NNPIHANDLE;
  hostPtr_ = nullptr;
  copyCommand_ = NNPI_INVALID_NNPIHANDLE;
  partialSize_ = 0;
  usage_ = ResourceUsage::None;
  deviceOptions_ = nullptr;
  cmdListIdx_ = UINT32_MAX;
}

NNPIResource::~NNPIResource() {
  if (copyCommand_ != NNPI_INVALID_NNPIHANDLE) {
    LOG_NNPI_INF_IF_ERROR(nnpiCopyCommandDestroy(copyCommand_),
                          "Failed to destroy NNPI copy command");
  }
  if (deviceResource_ != NNPI_INVALID_NNPIHANDLE) {
    LOG_NNPI_INF_IF_ERROR(nnpiDeviceResourceDestroy(deviceResource_),
                          "Failed to destroy NNPI device resource");
  }
  if (hostResource_ != NNPI_INVALID_NNPIHANDLE) {
    LOG_NNPI_INF_IF_ERROR(nnpiHostResourceDestroy(hostResource_),
                          "Failed to destroy NNPI host resource");
  }
  // Do not destroy the adapter or device context as the Resource doesn't own
  // them but only keeps reference for it's usage.
}

bool NNPIResource::Init(const NNPIObjectName name,
                        const NNPIDeviceOptions *deviceOptions,
                        NNPIAdapter adapter, NNPIDeviceContext device,
                        const NNPIResourceDesc *desc,
                        NNPIResource::ResourceUsage usage) {
  if (name == nullptr || desc == nullptr || deviceOptions == nullptr) {
    return false;
  }
  if (deviceOptions->inferOnDevice && (adapter == NNPI_INVALID_NNPIHANDLE ||
                                       device == NNPI_INVALID_NNPIHANDLE)) {
    return false;
  }

  std::strncpy(name_, name, sizeof(NNPIObjectName));
  device_ = device;
  adapter_ = adapter;
  deviceOptions_ = deviceOptions;
  usage_ = usage;
  desc_ = *desc;
  if (!deviceOptions_->inferOnDevice) {
    // Handle stuff for ice ref (or compile only path).
    size_t resourceSize = CalcDescSize(&desc_);
    LOG_AND_RETURN_IF(ERROR, resourceSize == 0,
                      "Failed to calculate resource size", false);
    refStorage_.resize(resourceSize);
    if (refStorage_.size() != resourceSize) {
      LOG_ERROR_IF_NOT(0) << "Failed to allocate memory for reference storage";
      return false;
    }
    hostPtr_ = &(refStorage_[0]);
    return true;
  }

  // Create host resource (pinned and aligned allocation).
  NNPIResourceDesc hostResDesc =
      desc_; // Make a copy of the desc to overwrite attributes.
  if (deviceOptions_->enabledCommandLists > 0) {
    hostResDesc.hostAttrib.locklessExecution =
        1; // Set host resource to lockless.
  }
  if (usage != ResourceUsage::StaticInputResource) {
    // No host resource needed for static inputs.
    LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
        nnpiHostResourceCreate(adapter_, &hostResDesc, &hostResource_),
        "Failed to create NNPI host resource");

    // Query host resource for address (not locking).
    LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
        nnpiHostResourceLock(hostResource_, NNPI_LOCKLESS_QUERY_ADDRESS,
                             UINT32_MAX, &hostPtr_),
        "Failed to lock host resource");
    memset(hostPtr_, 0,
           CalcDescSize(&desc_)); // Clear host resource to zero for compile
                                  // only path (USE_ICE_T).
  }

  // Create device resource.
  LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
      nnpiDeviceResourceCreate(device_, &desc_, &deviceResource_),
      "Failed to create NNPI device resource");

  // Create copy command.
  switch (usage_) {
  case ResourceUsage::InputResource:
    LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
        nnpiCopyCommandCreateHostToDevice(device_, deviceResource_,
                                          hostResource_, &copyCommand_),
        "Failed to create NNPI copy command (input)");
    break;
  case ResourceUsage::OutputResource:
    LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
        nnpiCopyCommandCreateDeviceToHost(device_, hostResource_,
                                          deviceResource_, &copyCommand_),
        "Failed to create NNPI copy command (output)");
    break;
  case ResourceUsage::StaticInputResource:
    // Fallthrough.
  case ResourceUsage::None:
    // Do nothing - no copy command needed.
    break;
  default:
    LOG_ERROR_IF_NOT(0) << "Invalid resource usage";
    return false;
  }

  return true;
}

NNPIInferenceErrorCode NNPIResource::PreInference(Tensor *t,
                                                  bool partialTensor) {
  if (usage_ != ResourceUsage::InputResource) {
    // Nothing to do here yet.
    return NNPI_INF_NO_ERROR;
  }

  // Update the host resource from the tensor content.
  UpdateHostResourceFromTensor(t, partialTensor);

  if (deviceOptions_->dumpIOtoFiles) {
    size_t unpaddedSize = t->getUnpaddedSizeInBytes();
    const bool downcastInt64 = t->getElementType() == glow::ElemKind::Int64ITy;
    if (downcastInt64) {
      unpaddedSize /= 2;
    }
    DumpToFile(std::string("input_") + std::string(name_) + ".txt", hostPtr_,
               unpaddedSize);
  }

  if (deviceOptions_->inferOnDevice &&
      (deviceOptions_->enabledCommandLists < 1)) {
    // Queue copy command separately (not using command lists).
    if (partialSize_) {
      NNPICopyCommandConfig cfg;
      memset(&cfg, 0, sizeof(NNPICopyCommandConfig));
      cfg.size = partialSize_;
      LOG_AND_RETURN_IF(ERROR, nnpiCopyCommandQueue(copyCommand_, &cfg),
                        "Failed to queue input copy command.",
                        NNPI_INF_UNKNOWN_ERROR);
    } else {
      LOG_AND_RETURN_IF(ERROR, nnpiCopyCommandQueue(copyCommand_, nullptr),
                        "Failed to queue input copy command.",
                        NNPI_INF_UNKNOWN_ERROR);
    }
  }
  return NNPI_INF_NO_ERROR;
}

NNPIInferenceErrorCode NNPIResource::PostInference(Tensor *t) {
  if (usage_ != ResourceUsage::OutputResource) {
    // Nothing to do here yet.
    return NNPI_INF_NO_ERROR;
  }

  if (deviceOptions_->inferOnDevice &&
      (deviceOptions_->enabledCommandLists < 2)) {
    // Lock and unlock to wait for inference completion.
    LOG_AND_RETURN_IF(ERROR,
                      nnpiHostResourceLock(hostResource_, NNPI_LOCK_FOR_READ,
                                           UINT32_MAX, &hostPtr_),
                      "Failed to lock host resource (output)",
                      NNPI_INF_UNKNOWN_ERROR);
    LOG_AND_RETURN_IF(ERROR, nnpiHostResourceUnlock(hostResource_),
                      "Failed to unlock host resource (output)",
                      NNPI_INF_UNKNOWN_ERROR);
  }

  char *tensorData = t->getUnsafePtr();
  const bool upcastInt64 = t->getElementType() == glow::ElemKind::Int64ITy;
  size_t unpaddedSize = t->getUnpaddedSizeInBytes();
  if (upcastInt64) {
    // TODO: add AVX implementation.
    const size_t outputSize = unpaddedSize / t->getType().getElementSize();
    int64_t *i64Data = reinterpret_cast<int64_t *>(tensorData);
    int32_t *i32Data = reinterpret_cast<int32_t *>(hostPtr_);
    for (size_t j = 0; j < outputSize; j++) {
      i64Data[j] = static_cast<int64_t>(i32Data[j]);
    }
  } else {
    std::memcpy(tensorData, hostPtr_, unpaddedSize);
  }

  if (deviceOptions_->dumpIOtoFiles) {
    DumpToFile(std::string("output_") + std::string(name_) + ".txt", hostPtr_,
               unpaddedSize);
  }
  return NNPI_INF_NO_ERROR;
}

bool NNPIResource::UpdateResourceDescFromTensorDesc(
    NNPIResourceDesc *rDesc, const NNPITensorDesc *tDesc) {
  if (tDesc == nullptr || rDesc == nullptr) {
    return false;
  }
  memset(rDesc, 0, sizeof(NNPIResourceDesc));
  rDesc->numDims = tDesc->numDims;
  for (size_t i = 0; i < rDesc->numDims; i++) {
    rDesc->dims[i] = tDesc->dims[i];
  }
  switch (tDesc->quantParams.precision) {
  case NNPI_PRECISION_FLOAT32:
    rDesc->dataType = NNPI_INF_PRECISION_FLOAT32;
    break;
  case NNPI_PRECISION_FLOAT16:
    rDesc->dataType = NNPI_INF_PRECISION_FLOAT16;
    break;
  case NNPI_PRECISION_INT32:
    rDesc->dataType = NNPI_INF_PRECISION_INT32;
    break;
  case NNPI_PRECISION_UINT32:
    rDesc->dataType = NNPI_INF_PRECISION_UINT32;
    break;
  case NNPI_PRECISION_INT8:
    rDesc->dataType = NNPI_INF_PRECISION_INT8;
    break;
  case NNPI_PRECISION_UINT8:
    rDesc->dataType = NNPI_INF_PRECISION_UINT8;
    break;
  case NNPI_PRECISION_BOOLEAN:
    rDesc->dataType = NNPI_INF_PRECISION_UINT8;
    break;
  default:
    LOG_AND_RETURN_IF(ERROR, true, "Unhandled precision", false);
    break;
  }

  return true;
}

void NNPIResource::UpdateDeviceResourceFromTensor(
    Tensor *t, std::function<void(Error)> resultCB) {
  LOG_AND_FAIL_CALLBACK_IF_NOT(
      t != nullptr, "Invalid tensor used to update static input", resultCB);

  if (deviceOptions_->inferOnDevice) {
    // Create host resource (and update hostPtr_).
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
        nnpiHostResourceCreate(adapter_, &desc_, &hostResource_),
        "Failed to create NNPI host resource", resultCB);

    // Lock/Unlock host resource and keep host address.
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
        nnpiHostResourceLock(hostResource_, NNPI_LOCK_FOR_WRITE, UINT32_MAX,
                             &hostPtr_),
        "Failed to lock host resource", resultCB);
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiHostResourceUnlock(hostResource_),
                                       "Failed to unlock host resource",
                                       resultCB);

    // Create copy command.
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
        nnpiCopyCommandCreateHostToDevice(device_, deviceResource_,
                                          hostResource_, &copyCommand_),
        "Failed to create NNPI copy command (input)", resultCB);
  } else {
    refStorage_.resize(t->getSizeInBytes());
    hostPtr_ = &(refStorage_.at(0));
  }

  LOG_AND_FAIL_CALLBACK_IF_NOT(
      t->getSizeInBytes() == t->getUnpaddedSizeInBytes(),
      "Static partial tensors are not supported", resultCB);
  // Copy data from tensor to host resource (convert if needed).
  UpdateHostResourceFromTensor(t, false);

  if (deviceOptions_->inferOnDevice) {
    // TODO: move to stream once exposed in the inference.
    if (deviceOptions_->enabledCommandLists > 0) {
      NNPICommandHandle ch;
      ch.type = NNPI_COMMAND_TYPE_COPY;
      ch.copyCommand = copyCommand_;
      NNPICommandList cl;
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
          nnpiCommandListCreate(&ch, 1, nullptr, 0, &cl),
          "Failed to create NNPI command list", resultCB);
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiCommandListQueue(cl, nullptr, 0),
                                         "Failed to queue command list",
                                         resultCB);
      uint32_t numErrors = 0;
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
          nnpiCommandListWait(cl, UINT32_MAX, nullptr, 0, &numErrors),
          "Failed to wait on command list completion", resultCB);
      LOG_AND_FAIL_CALLBACK_IF_NOT(numErrors == 0,
                                   "Command list returned errors", resultCB);
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiCommandListDestroy(cl),
                                         "Failed to destroy command list",
                                         resultCB);
      // TODO: dump errors generated in this command list
    } else {
      // No command lists.
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
          nnpiCopyCommandQueue(copyCommand_, nullptr),
          "Failed to queue input copy command.", resultCB);
      // Lock to make sure copy has ended
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(
          nnpiHostResourceLock(hostResource_, NNPI_LOCK_FOR_WRITE, UINT32_MAX,
                               &hostPtr_),
          "Failed to lock host resource", resultCB);
      LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiHostResourceUnlock(hostResource_),
                                         "Failed to unlock host resource",
                                         resultCB);
    }
    // Destroy host resource, copy command, command list.
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiCopyCommandDestroy(copyCommand_),
                                       "Failed to destroy NNPI copy command",
                                       resultCB);
    copyCommand_ = NNPI_INVALID_NNPIHANDLE;
    LOG_AND_CALLBACK_NNPI_INF_IF_ERROR(nnpiHostResourceDestroy(hostResource_),
                                       "Failed to destroy NNPI host resource",
                                       resultCB);
    hostResource_ = NNPI_INVALID_NNPIHANDLE;
    hostPtr_ = nullptr;
  }
  resultCB(Error::success());
}

void NNPIResource::UpdateHostResourceFromTensor(Tensor *t, bool partialTensor) {
  // Prepare data on the host resource (for ice-ref use int32sTorage).
  char *tensorData = t->getUnsafePtr();
  const bool downcastInt64 = t->getElementType() == glow::ElemKind::Int64ITy;
  size_t paddedSize = t->getSizeInBytes();
  size_t unpaddedSize = t->getUnpaddedSizeInBytes();

  if (downcastInt64) {
    paddedSize /= 2;
    unpaddedSize /= 2;
  }

  // Copy or convert.
  if (downcastInt64) { // Convert
    ConvertI64toI32(reinterpret_cast<const int64_t *>(tensorData),
                    reinterpret_cast<int32_t *>(hostPtr_),
                    unpaddedSize / sizeof(int32_t));
  } else { // Copy
    memcpy(hostPtr_, tensorData, unpaddedSize);
  }

  // Pad with zeros if needed.
  const bool partialData = (unpaddedSize != paddedSize);
  if (partialData && !partialTensor) {
    memset(reinterpret_cast<uint8_t *>(hostPtr_) + unpaddedSize, 0,
           paddedSize - unpaddedSize);
  }

  // Update partial size.
  partialSize_ = (partialData && partialTensor) ? unpaddedSize : 0;
}

std::string NNPIResource::Dump() const {
  std::stringstream stream;
  stream << "NNPIResource: " << name_;
  stream << ", DescSize: " << CalcDescSize(&desc_);
  stream << ", Usage: " << static_cast<int>(usage_);
  stream << ", Adapter: " << adapter_;
  stream << ", Device: " << device_;
  stream << ", DeviceResource: " << deviceResource_;
  stream << ", HostResource: " << hostResource_;
  stream << ", HostPtr: " << hostPtr_;
  stream << ", CopyCommand: " << copyCommand_;
  stream << ", CommandListIndex: " << cmdListIdx_;
  stream << ", PartialSize: " << partialSize_;
  return stream.str();
}

} // namespace runtime
} // namespace glow