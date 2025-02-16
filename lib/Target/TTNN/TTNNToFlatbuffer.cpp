// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ttmlir/Target/TTNN/TTNNToFlatbuffer.h"

#include "ttmlir/Dialect/TT/IR/TT.h"
#include "ttmlir/Dialect/TT/IR/TTOpsTypes.h"
#include "ttmlir/Dialect/TTIR/IR/TTIROps.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernel.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernelOps.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernelOpsTypes.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOps.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOpsAttrs.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOpsTypes.h"
#include "ttmlir/Dialect/TTNN/Transforms/Passes.h"
#include "ttmlir/Dialect/TTNN/Transforms/TTNNToCpp.h"
#include "ttmlir/Target/Common/Target.h"
#include "ttmlir/Target/Common/types_generated.h"
#include "ttmlir/Target/TTNN/Target.h"
#include "ttmlir/Target/TTNN/binary_generated.h"
#include "ttmlir/Target/TTNN/program_generated.h"
#include "ttmlir/Target/TTNN/utils.h"
#include "ttmlir/Target/Utils/FlatbufferObjectCache.h"
#include "ttmlir/Target/Utils/FuncOpToProgram.h"
#include "ttmlir/Target/Utils/MLIRToFlatbuffer.h"
#include "ttmlir/Version.h"

#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir::tt {

::tt::target::TensorMemoryLayout
toFlatbuffer(FlatbufferObjectCache &,
             ttnn::TensorMemoryLayoutAttr memLayoutAttr) {
  if (!memLayoutAttr) {
    return ::tt::target::TensorMemoryLayout::None;
  }

  switch (memLayoutAttr.getValue()) {
  case ttnn::TensorMemoryLayout::SingleBank:
    return ::tt::target::TensorMemoryLayout::SingleBank;
  case ttnn::TensorMemoryLayout::Interleaved:
    return ::tt::target::TensorMemoryLayout::Interleaved;
  case ttnn::TensorMemoryLayout::HeightSharded:
    return ::tt::target::TensorMemoryLayout::HeightSharded;
  case ttnn::TensorMemoryLayout::WidthSharded:
    return ::tt::target::TensorMemoryLayout::WidthSharded;
  case ttnn::TensorMemoryLayout::BlockSharded:
    return ::tt::target::TensorMemoryLayout::BlockSharded;
  }
}

::tt::target::MemorySpace toFlatbuffer(FlatbufferObjectCache &,
                                       ttnn::BufferType bufferType) {
  switch (bufferType) {
  case ttnn::BufferType::SystemMemory:
    return ::tt::target::MemorySpace::System;
  case ttnn::BufferType::DRAM:
    return ::tt::target::MemorySpace::DeviceDRAM;
  case ttnn::BufferType::L1:
    return ::tt::target::MemorySpace::DeviceL1;
  default:
    llvm_unreachable("unhandled buffer type");
  }
}

flatbuffers::Offset<::tt::target::MemoryDesc>
memrefAttrToFlatbuffer(FlatbufferObjectCache &cache, mlir::MemRefType memref,
                       ttnn::TensorMemoryLayoutAttr memLayoutAttr) {
  auto shapeInt64 = memref.getShape();
  std::vector<int32_t> shape(shapeInt64.begin(), shapeInt64.end());
  DataType dtype = DataType::Float32;
  ::tt::target::Dim2d tileShape(1, 1);
  mlir::Type elementType = memref.getElementType();
  std::uint64_t elementSize = 0;
  if (mlir::isa<TileType>(elementType)) {
    auto tileType = mlir::cast<TileType>(elementType);
    dtype = tileType.getDataType();
    tileShape = ::tt::target::Dim2d(tileType.getHeight(), tileType.getWidth());
    elementSize = tileType.getSizeBytes();
  } else {
    dtype = elementTypeToDataType(elementType);
    elementSize = getElementSizeBytes(dtype);
  }

  std::uint64_t size = elementSize;
  for (auto dim : shapeInt64) {
    size *= dim;
  }

  return ::tt::target::CreateMemoryDescDirect(
      *cache.fbb, &shape, &tileShape, toFlatbuffer(cache, dtype),
      toFlatbuffer(
          cache,
          mlir::cast<ttnn::BufferTypeAttr>(memref.getMemorySpace()).getValue()),
      toFlatbuffer(cache, memLayoutAttr), size);
}

flatbuffers::Offset<::tt::target::LayoutDesc> ttnnLayoutAttrToFlatbuffer(
    FlatbufferObjectCache &cache, ttnn::TTNNLayoutAttr layoutAttr,
    mlir::ArrayRef<int64_t> logicalShape, DeviceAttr deviceAttr) {
  auto coreRangeSet =
      toFlatbuffer(cache, layoutAttr.getGrid(), deviceAttr.getWorkerGrid());
  return ::tt::target::CreateLayoutDescDirect(
      *cache.fbb, toFlatbuffer(cache, OOBVal::Undef), &coreRangeSet,
      cache.getOrCreate(layoutAttr.getMemref(), memrefAttrToFlatbuffer,
                        layoutAttr.getMemLayout()));
}
} // namespace mlir::tt

namespace mlir::tt::ttnn {

constexpr uint64_t kHostAllocatedSize = 0;
constexpr uint64_t kHostAllocatedAddress = 0;

#define GEN_PASS_DEF_TTNNSERIALIZETOBINARY
#include "ttmlir/Dialect/TTNN/Transforms/Passes.h.inc"

::flatbuffers::Offset<::tt::target::ShardSpec>
shardSpecToFlatbuffer(FlatbufferObjectCache &cache,
                      ::mlir::tt::ttnn::ShardSpecAttr shardSpec) {
  llvm::ArrayRef<int64_t> shardShapeArr = shardSpec.getShardShape().getShape();
  std::vector<int64_t> shardShapeVec(shardShapeArr.begin(),
                                     shardShapeArr.end());
  auto shardShape = cache.fbb->CreateVector<int64_t>(shardShapeVec);
  return ::tt::target::CreateShardSpec(*cache.fbb, shardShape);
}

::flatbuffers::Offset<::tt::target::MemoryConfigDesc>
memoryConfigToFlatbuffer(FlatbufferObjectCache &cache,
                         ::mlir::tt::ttnn::MemoryConfigAttr memoryConfig) {
  ::tt::target::TensorMemoryLayout tensorMemoryLayout =
      ::tt::mlir::ttnn::utils::toTargetTensorMemoryLayout(
          memoryConfig.getTensorMemoryLayout().getValue());
  ::tt::target::BufferType bufferType =
      ::tt::mlir::ttnn::utils::toTargetBufferType(
          memoryConfig.getBufferType().getValue());
  auto shardSpec =
      cache.getOrCreate(memoryConfig.getShardSpec(), shardSpecToFlatbuffer);
  ::flatbuffers::Offset<::tt::target::MemoryConfigDesc> memoryConfigDesc =
      ::tt::target::CreateMemoryConfigDesc(*cache.fbb, tensorMemoryLayout,
                                           bufferType, shardSpec);
  return memoryConfigDesc;
}

::flatbuffers::Offset<::tt::target::DeviceRef>
createDeviceRef(FlatbufferObjectCache &cache, Value device) {
  auto deviceType = mlir::cast<DeviceType>(device.getType());
  auto chipIds = deviceType.getDesc().getChipIds();
  return ::tt::target::CreateDeviceRef(*cache.fbb, chipIds[0]);
}

template <typename OpT>
::flatbuffers::Offset<::tt::target::ttnn::Operation>
createOperation(FlatbufferObjectCache &cache, ::flatbuffers::Offset<OpT> op,
                std::string const &debugString, std::string const &locInfo) {
  return CreateOperationDirect(
      *cache.fbb, ::tt::target::ttnn::OpTypeTraits<OpT>::enum_value, op.Union(),
      debugString.c_str(), locInfo.c_str());
}

::flatbuffers::Offset<::tt::target::ttnn::GetDeviceOp>
createOp(FlatbufferObjectCache &cache, GetDeviceOp op) {
  auto result = op.getResult();
  auto resultType = mlir::cast<DeviceType>(result.getType());
  auto meshShape = resultType.getDesc().getMeshShape();
  auto meshVolume = ttmlir::utils::volume(meshShape);
  ::tt::target::Dim2d mesh;
  if (meshVolume > 1) {
    mesh = ::tt::target::Dim2d(meshShape[0], meshShape[1]);
  } else {
    mesh = ::tt::target::Dim2d(1, 1);
  }

  auto chipIds = toFlatbuffer(cache, resultType.getDesc().getChipIds());
  auto out = cache.getOrCreate(result, createDeviceRef);
  return ::tt::target::ttnn::CreateGetDeviceOp(*cache.fbb, &mesh, chipIds, out);
}

::flatbuffers::Offset<::tt::target::ttnn::ToMemoryConfigOp>
createOp(FlatbufferObjectCache &cache, ToMemoryConfigOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));

  auto memoryConfigDesc =
      cache.getOrCreate(op.getMemoryConfig(), memoryConfigToFlatbuffer);

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  return ::tt::target::ttnn::CreateToMemoryConfigOp(*cache.fbb, input,
                                                    memoryConfigDesc, output);
}

::flatbuffers::Offset<::tt::target::ttnn::ToLayoutOp>
createOp(FlatbufferObjectCache &cache, ToLayoutOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  ::tt::target::TensorLayout layout =
      ::tt::mlir::ttnn::utils::toTargetTensorLayout(op.getLayout());
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  std::optional<::mlir::tt::DataType> dtype = op.getDtype();
  std::optional<::mlir::tt::ttnn::MemoryConfigAttr> memoryConfig =
      op.getMemoryConfig();
  ::mlir::Value device = op.getDevice();
  if (device) {
    device = getOperandThroughDPSOps(device);
  }
  return ::tt::target::ttnn::CreateToLayoutOp(
      *cache.fbb, input, layout,
      dtype.has_value()
          ? ::flatbuffers::Optional<::tt::target::DataType>(
                ::tt::mlir::ttnn::utils::toTargetDataType(dtype.value()))
          : ::flatbuffers::nullopt,
      memoryConfig ? cache.getOrCreate(*memoryConfig, memoryConfigToFlatbuffer)
                   : 0,
      device ? cache.at<::tt::target::DeviceRef>(device) : 0, output);
}

::flatbuffers::Offset<::tt::target::ttnn::ToDTypeOp>
createOp(FlatbufferObjectCache &cache, ToDTypeOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  ::tt::target::DataType dtype =
      ::tt::mlir::ttnn::utils::toTargetDataType(op.getDtype());
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateToDTypeOp(*cache.fbb, input, dtype, output);
}

::flatbuffers::Offset<::tt::target::ttnn::TypecastOp>
createOp(FlatbufferObjectCache &cache, TypecastOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  ::tt::target::DataType dtype =
      ::tt::mlir::ttnn::utils::toTargetDataType(op.getDtype());
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateTypecastOp(*cache.fbb, input, dtype, output);
}

::flatbuffers::Offset<::tt::target::ttnn::ToDeviceOp>
createOp(FlatbufferObjectCache &cache, ToDeviceOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto device = getOperandThroughDPSOps(op.getDevice());

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  if (!op.getMemoryConfig()) {
    return ::tt::target::ttnn::CreateToDeviceOp(
        *cache.fbb, input, cache.at<::tt::target::DeviceRef>(device),
        /* memoryConfigDesc */ 0, output);
  }

  auto memoryConfigDesc =
      cache.getOrCreate(op.getMemoryConfig().value(), memoryConfigToFlatbuffer);

  return ::tt::target::ttnn::CreateToDeviceOp(
      *cache.fbb, input, cache.at<::tt::target::DeviceRef>(device),
      memoryConfigDesc, output);
}

::flatbuffers::Offset<::tt::target::ttnn::FromDeviceOp>
createOp(FlatbufferObjectCache &cache, FromDeviceOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateFromDeviceOp(*cache.fbb, input, output);
}

::flatbuffers::Offset<::tt::target::DistributionStrategy>
createDistributionStrategy(FlatbufferObjectCache &cache,
                           const Value &deviceValue,
                           const RankedTensorType &type, uint32_t &numShards) {
  auto noneDistributionStrategy = [&cache]() {
    ::flatbuffers::Offset<void> distribution = 0;
    return ::tt::target::CreateDistributionStrategy(
        *cache.fbb, ::tt::target::DistributedTensorConfig::NONE, distribution);
  };

  if (!deviceValue) {
    return noneDistributionStrategy();
  }

  auto deviceOp = mlir::cast<GetDeviceOp>(
      getOperandThroughDPSOps(deviceValue).getDefiningOp());
  auto resultType = mlir::cast<DeviceType>(deviceOp.getResult().getType());
  ::llvm::ArrayRef<int64_t> meshShape = resultType.getDesc().getMeshShape();
  numShards = ttmlir::utils::volume(meshShape);

  if (numShards == 1) {
    return noneDistributionStrategy();
  }

  assert(meshShape.size() <= 2 && "expected 2D mesh shape");

  // One-dimensional tensor sharding strategy. Tensor is sliced by the number of
  // devices at a certain dimension. For EmptyOp and FullOp, we assume that the
  // tensor is sliced at the fastest dimension.
  if (meshShape[0] == 1 || meshShape[1] == 1) {
    assert(type.getShape().size() > 0 && "expected non-zero tensor shape");
    uint32_t target_dim = type.getShape().size() - 1;
    auto strategy = ::tt::target::CreateShardTensor(*cache.fbb, target_dim);
    return ::tt::target::CreateDistributionStrategy(
        *cache.fbb, ::tt::target::DistributedTensorConfig::ShardTensor,
        strategy.Union());
  }

  const ::tt::target::Dim2d shard_mesh(meshShape[0], meshShape[1]);
  auto strategy = ::tt::target::CreateShardTensor2D(*cache.fbb, &shard_mesh);
  return ::tt::target::CreateDistributionStrategy(
      *cache.fbb, ::tt::target::DistributedTensorConfig::ShardTensor2D,
      strategy.Union());
}

::flatbuffers::Offset<::tt::target::ttnn::EmptyOp>
createOp(FlatbufferObjectCache &cache, EmptyOp op) {
  ::llvm::ArrayRef<int64_t> shape = op.getShape().getShape();
  ::tt::target::DataType dtype =
      ::tt::mlir::ttnn::utils::toTargetDataType(op.getDtype());
  ::tt::target::TensorLayout layout =
      ::tt::mlir::ttnn::utils::toTargetTensorLayout(op.getLayout());

  uint32_t numShards = 1;
  auto strategy = createDistributionStrategy(
      cache, op.getDevice(), mlir::cast<RankedTensorType>(op.getType()),
      numShards);
  auto output = getOperandThroughDPSOps(op.getResult());

  auto device = getOperandThroughDPSOps(op.getDevice());

  auto memoryConfigDesc =
      cache.getOrCreate(op.getMemoryConfig(), memoryConfigToFlatbuffer);

  return ::tt::target::ttnn::CreateEmptyOp(
      *cache.fbb, cache.fbb->CreateVector<int64_t>(shape), dtype, layout,
      numShards, cache.at<::tt::target::DeviceRef>(device), memoryConfigDesc,
      strategy,
      cache.getOrCreate(output, tensorValueToFlatbuffer, kHostAllocatedAddress,
                        kHostAllocatedSize));
}

::flatbuffers::Offset<::tt::target::ttnn::FullOp>
createOp(FlatbufferObjectCache &cache, FullOp op) {
  auto device = getOperandThroughDPSOps(op.getDevice());
  auto fillValue = op.getFillValue().convertToFloat();
  auto output = getOperandThroughDPSOps(op.getResult());
  uint32_t numShards = 1;
  auto strategy = createDistributionStrategy(
      cache, op.getDevice(), mlir::cast<RankedTensorType>(op.getType()),
      numShards);
  return ::tt::target::ttnn::CreateFullOp(
      *cache.fbb, cache.at<::tt::target::DeviceRef>(device), fillValue,
      numShards, strategy,
      cache.getOrCreate(output, tensorValueToFlatbuffer, kHostAllocatedAddress,
                        kHostAllocatedSize));
}

::flatbuffers::Offset<::tt::target::ttnn::ArangeOp>
createOp(FlatbufferObjectCache &cache, ArangeOp op) {

  std::optional<::tt::target::DataType> dtype =
      op.getDtype().has_value()
          ? std::make_optional(toFlatbuffer(cache, op.getDtype().value()))
          : std::nullopt;
  auto device =
      op.getDevice() ? cache.at<::tt::target::DeviceRef>(op.getDevice()) : 0;

  auto memoryConfigDesc = op.getMemoryConfig().has_value()
                              ? cache.getOrCreate(op.getMemoryConfig().value(),
                                                  memoryConfigToFlatbuffer)
                              : 0;

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateArangeOp(
      *cache.fbb, static_cast<float>(op.getStart()),
      static_cast<float>(op.getEnd()), static_cast<float>(op.getStep()),
      dtype /* optional */, device /* optional */,
      memoryConfigDesc /* optional */, output);
}

::flatbuffers::Offset<::tt::target::ttnn::ZerosOp>
createOp(FlatbufferObjectCache &cache, ZerosOp op) {
  ::flatbuffers::Offset<::flatbuffers::Vector<int64_t>> shape =
      cache.fbb->CreateVector<int64_t>(op.getShape().getShape());

  ::flatbuffers::Optional<::tt::target::DataType> dtype =
      toFlatbufferOptional(cache, op.getDtype());

  ::flatbuffers::Optional<::tt::target::TensorLayout> layout =
      toFlatbufferOptional(cache, op.getLayout());

  flatbuffers::Offset<::tt::target::DeviceRef> device =
      op.getDevice() ? cache.at<::tt::target::DeviceRef>(op.getDevice()) : 0;

  auto memoryConfigDesc = op.getMemoryConfig().has_value()
                              ? cache.getOrCreate(op.getMemoryConfig().value(),
                                                  memoryConfigToFlatbuffer)
                              : 0;

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateZerosOp(*cache.fbb, shape, dtype, layout,
                                           device, memoryConfigDesc, output);
}

::flatbuffers::Offset<::tt::target::ttnn::OnesOp>
createOp(FlatbufferObjectCache &cache, OnesOp op) {
  ::flatbuffers::Offset<::flatbuffers::Vector<int64_t>> shape =
      cache.fbb->CreateVector<int64_t>(op.getShape().getShape());

  ::flatbuffers::Optional<::tt::target::DataType> dtype =
      toFlatbufferOptional(cache, op.getDtype());

  ::flatbuffers::Optional<::tt::target::TensorLayout> layout =
      toFlatbufferOptional(cache, op.getLayout());

  flatbuffers::Offset<::tt::target::DeviceRef> device =
      op.getDevice() ? cache.at<::tt::target::DeviceRef>(op.getDevice()) : 0;

  auto memoryConfigDesc = op.getMemoryConfig().has_value()
                              ? cache.getOrCreate(op.getMemoryConfig().value(),
                                                  memoryConfigToFlatbuffer)
                              : 0;

  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateOnesOp(*cache.fbb, shape, dtype, layout,
                                          device, memoryConfigDesc, output);
}

::flatbuffers::Offset<::tt::target::ttnn::LinearOp>
createOp(FlatbufferObjectCache &cache, LinearOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getA()));
  auto in1 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getB()));
  auto bias = op.getODSOperands(2).empty()
                  ? flatbuffers::Offset<::tt::target::TensorRef>()
                  : cache.at<::tt::target::TensorRef>(
                        getOperandThroughDPSOps(op.getBias()));
  auto output = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  return ::tt::target::ttnn::CreateLinearOp(*cache.fbb, in0, in1, bias, output);
}

// ANCHOR: adding_an_op_matmul_serialize_to_binary
::flatbuffers::Offset<::tt::target::ttnn::MatmulOp>
createOp(FlatbufferObjectCache &cache, MatmulOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getA()));
  auto in1 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getB()));
  auto output = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  return ::tt::target::ttnn::CreateMatmulOp(*cache.fbb, in0, in1, output);
}
// ANCHOR_END: adding_an_op_matmul_serialize_to_binary

::flatbuffers::Offset<::tt::target::ttnn::MorehCumSumOp>
createOp(FlatbufferObjectCache &cache, MorehCumSumOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  auto memoryConfigDesc =
      op.getMemoryConfig()
          ? cache.getOrCreate(*op.getMemoryConfig(), memoryConfigToFlatbuffer)
          : 0;

  return ::tt::target::ttnn::CreateMorehCumSumOp(*cache.fbb, in, output,
                                                 op.getDim(), memoryConfigDesc);
}

::flatbuffers::Offset<::tt::target::ttnn::Conv2dOp>
createOp(FlatbufferObjectCache &cache, Conv2dOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto in1 = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getWeight()));
  auto in2 = op.getODSOperands(2).empty()
                 ? flatbuffers::Offset<::tt::target::TensorRef>()
                 : cache.at<::tt::target::TensorRef>(
                       getOperandThroughDPSOps(op.getBias()));
  auto output = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));

  auto device = getOperandThroughDPSOps(op.getDevice());
  return ::tt::target::ttnn::CreateConv2dOp(
      *cache.fbb, in0, in1, in2, output,
      cache.at<::tt::target::DeviceRef>(device), op.getInChannels(),
      op.getOutChannels(), op.getBatchSize(), op.getInputHeight(),
      op.getInputWidth(), op.getKernelHeight(), op.getKernelWidth(),
      op.getStrideHeight(), op.getStrideWidth(), op.getPaddingHeight(),
      op.getPaddingWidth(), op.getDilationHeight(), op.getDilationWidth(),
      op.getGroups());
}

::flatbuffers::Offset<::tt::target::ttnn::ConvTranspose2dOp>
createOp(FlatbufferObjectCache &cache, ConvTranspose2dOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto in1 = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getWeight()));
  auto in2 = op.getODSOperands(2).empty()
                 ? flatbuffers::Offset<::tt::target::TensorRef>()
                 : cache.at<::tt::target::TensorRef>(
                       getOperandThroughDPSOps(op.getBias()));
  auto output = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));

  auto device = getOperandThroughDPSOps(op.getDevice());

  ::flatbuffers::Offset<::flatbuffers::Vector<int32_t>> kernelSize =
      toFlatbuffer(cache, op.getKernelSize());
  ::flatbuffers::Offset<::flatbuffers::Vector<int32_t>> stride =
      toFlatbuffer(cache, op.getStride());
  ::flatbuffers::Offset<::flatbuffers::Vector<int32_t>> padding =
      toFlatbuffer(cache, op.getPadding());
  ::flatbuffers::Offset<::flatbuffers::Vector<int32_t>> outputPadding =
      toFlatbuffer(cache, op.getOutputPadding());
  ::flatbuffers::Offset<::flatbuffers::Vector<int32_t>> dilation =
      toFlatbuffer(cache, op.getDilation());

  return ::tt::target::ttnn::CreateConvTranspose2dOp(
      *cache.fbb, in0, in1, in2, output,
      cache.at<::tt::target::DeviceRef>(device), op.getInChannels(),
      op.getOutChannels(), op.getBatchSize(), op.getInputHeight(),
      op.getInputWidth(), kernelSize, stride, padding, outputPadding, dilation,
      op.getGroups());
}

::flatbuffers::Offset<::tt::target::ttnn::AllGatherOp>
createOp(FlatbufferObjectCache &cache, AllGatherOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  auto device = getOperandThroughDPSOps(op.getDevice());
  return ::tt::target::ttnn::CreateAllGatherOp(
      *cache.fbb, input, output, cache.at<::tt::target::DeviceRef>(device),
      op.getDim(), op.getNumLinks());
}

::flatbuffers::Offset<::tt::target::ttnn::ReduceScatterOp>
createOp(FlatbufferObjectCache &cache, ReduceScatterOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  auto device = getOperandThroughDPSOps(op.getDevice());
  return ::tt::target::ttnn::CreateReduceScatterOp(
      *cache.fbb, input, output, cache.at<::tt::target::DeviceRef>(device),
      op.getScatterSplitDim(), static_cast<uint32_t>(op.getMathOp()),
      op.getNumLinks());
}

::flatbuffers::Offset<::tt::target::ttnn::MeshShardOp>
createOp(FlatbufferObjectCache &cache, MeshShardOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  auto device = getOperandThroughDPSOps(op.getDevice());
  const mlir::tt::MeshShardDirection shardDirection = op.getShardDirection();
  const mlir::tt::MeshShardType shardType = op.getShardType();
  llvm::ArrayRef<int64_t> shardShape = op.getShardShape();
  llvm::ArrayRef<int64_t> shardDims = op.getShardDims();

  ::tt::target::MeshShardDirection meshShardDirection;
  if (shardDirection == mlir::tt::MeshShardDirection::FullToShard) {
    meshShardDirection = ::tt::target::MeshShardDirection::FullToShardShape;
  } else if (shardDirection == mlir::tt::MeshShardDirection::ShardToFull) {
    meshShardDirection = ::tt::target::MeshShardDirection::ShardToFullShape;
  } else {
    llvm_unreachable("unhandled mesh_shard direction");
  }

  ::tt::target::MeshShardType meshShardType;
  if (shardType == mlir::tt::MeshShardType::Replicate) {
    meshShardType = ::tt::target::MeshShardType::Replicate;
  } else if (shardType == mlir::tt::MeshShardType::Devices) {
    meshShardType = ::tt::target::MeshShardType::Devices;
  } else {
    llvm_unreachable("unhandled mesh_shard type");
  }

  return ::tt::target::ttnn::CreateMeshShardOp(
      *cache.fbb, input, output, cache.at<::tt::target::DeviceRef>(device),
      meshShardDirection, meshShardType,
      cache.fbb->CreateVector<int64_t>(shardShape),
      cache.fbb->CreateVector<int64_t>(shardDims));
}

::flatbuffers::Offset<::tt::target::ttnn::PermuteOp>
createOp(FlatbufferObjectCache &cache, PermuteOp op) {
  flatbuffers::Offset<::tt::target::TensorRef> input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  flatbuffers::Offset<flatbuffers::Vector<int64_t>> permutation =
      toFlatbuffer(cache, op.getPermutation());
  std::optional<mlir::tt::ttnn::MemoryConfigAttr> memoryConfig =
      op.getMemoryConfig();
  float padValue = op.getPadValue().convertToFloat();
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  return ::tt::target::ttnn::CreatePermuteOp(
      *cache.fbb, input, permutation,
      memoryConfig ? cache.getOrCreate(*memoryConfig, memoryConfigToFlatbuffer)
                   : 0,
      padValue, output);
}

::flatbuffers::Offset<::tt::target::ttnn::UpsampleOp>
createOp(FlatbufferObjectCache &cache, UpsampleOp op) {
  flatbuffers::Offset<::tt::target::TensorRef> input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  flatbuffers::Offset<flatbuffers::String> mode =
      toFlatbuffer(cache, op.getMode());
  flatbuffers::Offset<::tt::target::MemoryConfigDesc> memoryConfig =
      op.getMemoryConfig()
          ? cache.getOrCreate(*op.getMemoryConfig(), memoryConfigToFlatbuffer)
          : 0;
  flatbuffers::Offset<::tt::target::TensorRef> output =
      cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                        kHostAllocatedAddress, kHostAllocatedSize);

  ::tt::target::ttnn::Scale2D scaleType;
  ::flatbuffers::Offset<void> scaleFactor;
  if (auto uniformScaleFactor =
          mlir::dyn_cast<IntegerAttr>(op.getScaleFactor())) {
    scaleType = ::tt::target::ttnn::Scale2D::UniformScale2D;
    scaleFactor = ::tt::target::ttnn::CreateUniformScale2D(
                      *cache.fbb, uniformScaleFactor.getSInt())
                      .Union();
  } else if (auto nonUniformScaleFactor =
                 mlir::dyn_cast<DenseI32ArrayAttr>(op.getScaleFactor())) {
    scaleType = ::tt::target::ttnn::Scale2D::NonUniformScale2D;
    scaleFactor =
        ::tt::target::ttnn::CreateNonUniformScale2D(
            *cache.fbb, toFlatbuffer(cache, nonUniformScaleFactor.asArrayRef()))
            .Union();
  } else {
    assert(false && "Unhandled scale factor type");
  }

  return ::tt::target::ttnn::CreateUpsampleOp(
      *cache.fbb, input, scaleType, scaleFactor, mode, memoryConfig, output);
}

template <typename EltwiseOp, typename EltwiseOpParams>
::flatbuffers::Offset<EltwiseOpParams>
createEltwiseOpParams(FlatbufferObjectCache &cache, EltwiseOp op) {
  if constexpr (std::is_same_v<EltwiseOp, ClampOp>) {
    auto min = op.getMin().convertToFloat();
    auto max = op.getMax().convertToFloat();
    return ::tt::target::ttnn::CreateClampOpParams(*cache.fbb, min, max);
  }
  if constexpr (std::is_same_v<EltwiseOp, LeakyReluOp>) {
    auto parameter = op.getParameter().convertToFloat();
    return ::tt::target::ttnn::CreateEltwiseOpWithFloatParams(*cache.fbb,
                                                              parameter);
  }
}

::flatbuffers::Offset<::tt::target::ttnn::UpdateCacheOp>
createOp(FlatbufferObjectCache &cache, UpdateCacheOp op) {
  auto cacheOperand =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getCache()));
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto updateIndex = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getUpdateIndex()));

  return ::tt::target::ttnn::CreateUpdateCacheOp(
      *cache.fbb, cacheOperand, input, updateIndex, op.getBatchOffset());
}

::flatbuffers::Offset<::tt::target::ttnn::FillCacheOp>
createOp(FlatbufferObjectCache &cache, FillCacheOp op) {
  auto cacheOperand =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getCache()));
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));

  return ::tt::target::ttnn::CreateFillCacheOp(*cache.fbb, cacheOperand, input,
                                               op.getBatchOffset());
}

template <typename EltwiseOp>
::flatbuffers::Offset<::tt::target::ttnn::EltwiseOp>
createNonDPSEltwiseOp(FlatbufferObjectCache &cache, EltwiseOp op) {
  ::tt::target::ttnn::EltwiseOpType type;
  ::tt::target::ttnn::EltwiseOpParams paramsType =
      ::tt::target::ttnn::EltwiseOpParams::NONE;
  ::flatbuffers::Offset<void> params = 0;
  if constexpr (std::is_same_v<EltwiseOp, ClampOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Clamp;
    paramsType = ::tt::target::ttnn::EltwiseOpParams::ClampOpParams;
    params = createEltwiseOpParams<ClampOp, ::tt::target::ttnn::ClampOpParams>(
                 cache, op)
                 .Union();
  } else {
    llvm_unreachable("unhandled non-DPS EltwiseOp");
  }

  std::vector<::flatbuffers::Offset<::tt::target::TensorRef>> ins;
  for (auto input : op.getInputs()) {
    ins.push_back(
        cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(input)));
  }
  assert(op.getResults().size() == 1);
  auto out = cache.getOrCreate(op.getResults().front(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);
  return ::tt::target::ttnn::CreateEltwiseOpDirect(*cache.fbb, type, &ins, out,
                                                   paramsType, params);
}

template <typename EltwiseOp>
::flatbuffers::Offset<::tt::target::ttnn::EltwiseOp>
createEltwiseOp(FlatbufferObjectCache &cache, EltwiseOp op) {
  ::tt::target::ttnn::EltwiseOpType type;
  ::tt::target::ttnn::EltwiseOpParams paramsType =
      ::tt::target::ttnn::EltwiseOpParams::NONE;
  ::flatbuffers::Offset<void> params = 0;
  if constexpr (std::is_same_v<EltwiseOp, AbsOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Abs;
  } else if constexpr (std::is_same_v<EltwiseOp, AddOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Add;
  } else if constexpr (std::is_same_v<EltwiseOp, CbrtOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Cbrt;
  } else if constexpr (std::is_same_v<EltwiseOp, FloorOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Floor;
  } else if constexpr (std::is_same_v<EltwiseOp, IsFiniteOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::IsFinite;
  } else if constexpr (std::is_same_v<EltwiseOp, LogicalAndOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LogicalAnd;
  } else if constexpr (std::is_same_v<EltwiseOp, LogicalNotOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LogicalNot;
  } else if constexpr (std::is_same_v<EltwiseOp, LogicalOrOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LogicalOr;
  } else if constexpr (std::is_same_v<EltwiseOp, LogicalXorOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LogicalXor;
  } else if constexpr (std::is_same_v<EltwiseOp, BitwiseAndOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::BitwiseAnd;
  } else if constexpr (std::is_same_v<EltwiseOp, BitwiseOrOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::BitwiseOr;
  } else if constexpr (std::is_same_v<EltwiseOp, BitwiseXorOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::BitwiseXor;
  } else if constexpr (std::is_same_v<EltwiseOp, BitwiseNotOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::BitwiseNot;
  } else if constexpr (std::is_same_v<EltwiseOp, MultiplyOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Multiply;
  } else if constexpr (std::is_same_v<EltwiseOp, NegOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Neg;
  } else if constexpr (std::is_same_v<EltwiseOp, SubtractOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Subtract;
  } else if constexpr (std::is_same_v<EltwiseOp, EqualOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Equal;
  } else if constexpr (std::is_same_v<EltwiseOp, NotEqualOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::NotEqual;
  } else if constexpr (std::is_same_v<EltwiseOp, GreaterEqualOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::GreaterEqual;
  } else if constexpr (std::is_same_v<EltwiseOp, GreaterThanOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::GreaterThan;
  } else if constexpr (std::is_same_v<EltwiseOp, LessEqualOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LessEqual;
  } else if constexpr (std::is_same_v<EltwiseOp, LessThanOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LessThan;
  } else if constexpr (std::is_same_v<EltwiseOp, MaximumOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Maximum;
  } else if constexpr (std::is_same_v<EltwiseOp, MinimumOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Minimum;
  } else if constexpr (std::is_same_v<EltwiseOp, ReluOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Relu;
  } else if constexpr (std::is_same_v<EltwiseOp, SqrtOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Sqrt;
  } else if constexpr (std::is_same_v<EltwiseOp, RsqrtOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Rsqrt;
  } else if constexpr (std::is_same_v<EltwiseOp, SignOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Sign;
  } else if constexpr (std::is_same_v<EltwiseOp, ReciprocalOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Reciprocal;
  } else if constexpr (std::is_same_v<EltwiseOp, DivOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Div;
  } else if constexpr (std::is_same_v<EltwiseOp, SigmoidOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Sigmoid;
  } else if constexpr (std::is_same_v<EltwiseOp, ScatterOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Scatter;
  } else if constexpr (std::is_same_v<EltwiseOp, Log1pOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Log1p;
  } else if constexpr (std::is_same_v<EltwiseOp, ExpOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Exp;
  } else if constexpr (std::is_same_v<EltwiseOp, CeilOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Ceil;
  } else if constexpr (std::is_same_v<EltwiseOp, CosOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Cos;
  } else if constexpr (std::is_same_v<EltwiseOp, SinOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Sin;
  } else if constexpr (std::is_same_v<EltwiseOp, LogOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Log;
  } else if constexpr (std::is_same_v<EltwiseOp, Expm1Op>) {
    type = ::tt::target::ttnn::EltwiseOpType::Expm1;
  } else if constexpr (std::is_same_v<EltwiseOp, RemainderOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Remainder;
  } else if constexpr (std::is_same_v<EltwiseOp, WhereOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Where;
  } else if constexpr (std::is_same_v<EltwiseOp, GeluOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Gelu;
  } else if constexpr (std::is_same_v<EltwiseOp, LeakyReluOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::LeakyRelu;
    paramsType = ::tt::target::ttnn::EltwiseOpParams::EltwiseOpWithFloatParams;
    params =
        createEltwiseOpParams<LeakyReluOp,
                              ::tt::target::ttnn::EltwiseOpWithFloatParams>(
            cache, op)
            .Union();
  } else if constexpr (std::is_same_v<EltwiseOp, TanOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Tan;
  } else if constexpr (std::is_same_v<EltwiseOp, TanhOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Tanh;
  } else if constexpr (std::is_same_v<EltwiseOp, PowerOp>) {
    type = ::tt::target::ttnn::EltwiseOpType::Power;
  } else {
    llvm_unreachable("unhandled EltwiseOp");
  }
  std::vector<::flatbuffers::Offset<::tt::target::TensorRef>> ins;
  for (auto input : op.getInputs()) {
    ins.push_back(
        cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(input)));
  }
  assert(op.getOutputs().size() == 1);
  return ::tt::target::ttnn::CreateEltwiseOpDirect(
      *cache.fbb, type, &ins,
      cache.at<::tt::target::TensorRef>(
          getOperandThroughDPSOps(op.getOutputs().front())),
      paramsType, params);
}

template <typename ReductionOp>
::flatbuffers::Offset<::tt::target::ttnn::ReductionOp>
createReductionOp(FlatbufferObjectCache &cache, ReductionOp op) {
  ::tt::target::ttnn::ReductionOpType type;
  if constexpr (std::is_same_v<ReductionOp, SumOp>) {
    type = ::tt::target::ttnn::ReductionOpType::Sum;
  } else if constexpr (std::is_same_v<ReductionOp, MeanOp>) {
    type = ::tt::target::ttnn::ReductionOpType::Mean;
  } else if constexpr (std::is_same_v<ReductionOp, MaxOp>) {
    type = ::tt::target::ttnn::ReductionOpType::Max;
  } else if constexpr (std::is_same_v<ReductionOp, MinOp>) {
    type = ::tt::target::ttnn::ReductionOpType::Min;
  } else {
    llvm_unreachable("unhandled ReductionOp");
  }

  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  auto dimArg =
      arrayAttrToFlatbuffer<mlir::IntegerAttr, int>(cache, op.getDimArg());

  return ::tt::target::ttnn::CreateReductionOp(*cache.fbb, type, in, output,
                                               dimArg, op.getKeepDim());
}

template <typename ReductionOp>
::flatbuffers::Offset<::tt::target::ttnn::ReductionProdOp>
createReductionProdOp(FlatbufferObjectCache &cache, ReductionOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto output = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                                  kHostAllocatedAddress, kHostAllocatedSize);
  auto memoryConfigDesc =
      op.getMemoryConfig()
          ? cache.getOrCreate(*op.getMemoryConfig(), memoryConfigToFlatbuffer)
          : 0;

  return ::tt::target::ttnn::CreateReductionProdOp(
      *cache.fbb, in, output, op.getAllDimensions(), op.getDimArg(),
      op.getKeepDim(), memoryConfigDesc);
}

::flatbuffers::Offset<::tt::target::ttnn::TransposeOp>
createTransposeOp(FlatbufferObjectCache &cache, TransposeOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto out = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);
  int32_t dim0 = op.getDim0();
  int32_t dim1 = op.getDim1();

  return ::tt::target::ttnn::CreateTransposeOp(*cache.fbb, in, out, dim0, dim1);
}

::flatbuffers::Offset<::tt::target::ttnn::ConcatOp>
createConcatOp(FlatbufferObjectCache &cache, ConcatOp op) {
  std::vector<::flatbuffers::Offset<::tt::target::TensorRef>> ins;
  for (auto input : op.getInputs()) {
    ins.push_back(
        cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(input)));
  }
  auto out = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  int32_t dim = op.getDim();

  std::optional<mlir::tt::ttnn::MemoryConfigAttr> memoryConfig =
      op.getMemoryConfig();

  return ::tt::target::ttnn::CreateConcatOpDirect(
      *cache.fbb, &ins, out, dim,
      memoryConfig ? cache.getOrCreate(*memoryConfig, memoryConfigToFlatbuffer)
                   : 0);
}

::flatbuffers::Offset<::tt::target::ttnn::EmbeddingOp>
createEmbeddingOp(FlatbufferObjectCache &cache, EmbeddingOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto in1 = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getWeight()));
  auto out = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  return ::tt::target::ttnn::CreateEmbeddingOp(*cache.fbb, in0, in1, out);
}

template <typename EmbeddingBackwardOp>
::flatbuffers::Offset<::tt::target::ttnn::EmbeddingBackwardOp>
createEmbeddingBackwardOp(FlatbufferObjectCache &cache,
                          EmbeddingBackwardOp op) {
  auto in0 =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto in1 = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getWeight()));
  auto in2 = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getInGradient()));
  std::optional<::mlir::tt::DataType> dtype = op.getDtype();
  std::optional<::mlir::tt::ttnn::MemoryConfigAttr> memoryConfig =
      op.getMemoryConfig();

  auto out = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  return ::tt::target::ttnn::CreateEmbeddingBackwardOp(
      *cache.fbb, in0, in1, in2,
      dtype.has_value()
          ? ::flatbuffers::Optional<::tt::target::DataType>(
                ::tt::mlir::ttnn::utils::toTargetDataType(dtype.value()))
          : ::flatbuffers::nullopt,
      memoryConfig ? cache.getOrCreate(*memoryConfig, memoryConfigToFlatbuffer)
                   : 0,
      out);
}

::flatbuffers::Offset<::tt::target::ttnn::ReshapeOp>
createReshapeOp(FlatbufferObjectCache &cache, ReshapeOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto shape =
      arrayAttrToFlatbuffer<mlir::IntegerAttr, int32_t>(cache, op.getShape());
  auto out = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateReshapeOp(*cache.fbb, in, out, shape);
}

template <typename RepeatOp>
::flatbuffers::Offset<::tt::target::ttnn::RepeatOp>
createRepeatOp(FlatbufferObjectCache &cache, RepeatOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  ::llvm::ArrayRef<int64_t> repeatDims = op.getRepeatDims().getShape();
  auto out = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);

  return ::tt::target::ttnn::CreateRepeatOp(
      *cache.fbb, in, out, cache.fbb->CreateVector<int64_t>(repeatDims));
}

::flatbuffers::Offset<::tt::target::ttnn::PadOp>
createPadOp(FlatbufferObjectCache &cache, PadOp op) {
  flatbuffers::Offset<::tt::target::TensorRef> in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  std::vector<uint32_t> padding(op.getPadding().begin(), op.getPadding().end());
  float value = op.getValue().convertToFloat();
  flatbuffers::Offset<::tt::target::TensorRef> out =
      cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                        kHostAllocatedAddress, kHostAllocatedSize);

  flatbuffers::Offset<::tt::target::MemoryConfigDesc> memoryConfigDesc =
      op.getMemoryConfig()
          ? cache.getOrCreate(*op.getMemoryConfig(), memoryConfigToFlatbuffer)
          : 0;
  return ::tt::target::ttnn::CreatePadOp(
      *cache.fbb, in, out, cache.fbb->CreateVector<uint32_t>(padding), value,
      op.getUseMulticore(), memoryConfigDesc);
}

::flatbuffers::Offset<::tt::target::ttnn::SliceOp>
createSliceOp(FlatbufferObjectCache &cache, SliceOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto out = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));
  auto begins =
      arrayAttrToFlatbuffer<mlir::IntegerAttr, int64_t>(cache, op.getBegins());
  auto ends =
      arrayAttrToFlatbuffer<mlir::IntegerAttr, int64_t>(cache, op.getEnds());
  auto step =
      arrayAttrToFlatbuffer<mlir::IntegerAttr, int64_t>(cache, op.getStep());

  return ::tt::target::ttnn::CreateSliceOp(*cache.fbb, in, out, begins, ends,
                                           step);
}

::flatbuffers::Offset<::tt::target::ttnn::MaxPool2dOp>
createMaxPool2dOp(FlatbufferObjectCache &cache, MaxPool2dOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto out = cache.at<::tt::target::TensorRef>(
      getOperandThroughDPSOps(op.getResult()));

  auto device = getOperandThroughDPSOps(op.getDevice());
  return ::tt::target::ttnn::CreateMaxPool2dOp(
      *cache.fbb, in, out, cache.at<::tt::target::DeviceRef>(device),
      op.getBatchSize(), op.getInputHeight(), op.getInputWidth(),
      op.getChannels(), op.getKernelHeight(), op.getKernelWidth(),
      op.getStrideHeight(), op.getStrideWidth(), op.getDilationHeight(),
      op.getDilationWidth(), op.getCeilMode(), op.getPaddingHeight(),
      op.getPaddingWidth());
}

::flatbuffers::Offset<::tt::target::ttnn::RepeatInterleaveOp>
createRepeatInterleaveOp(FlatbufferObjectCache &cache, RepeatInterleaveOp op) {
  auto input =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto out = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);
  std::optional<mlir::tt::ttnn::MemoryConfigAttr> memoryConfig =
      op.getMemoryConfig();
  uint32_t repeats = op.getRepeats();
  int32_t dim = op.getDim();
  return ::tt::target::ttnn::CreateRepeatInterleaveOp(
      *cache.fbb, input, out, repeats, dim,
      memoryConfig ? cache.getOrCreate(*memoryConfig, memoryConfigToFlatbuffer)
                   : 0);
}

::flatbuffers::Offset<::tt::target::ttnn::SoftmaxOp>
createSoftmaxOp(FlatbufferObjectCache &cache, SoftmaxOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto out = cache.getOrCreate(op.getResult(), tensorValueToFlatbuffer,
                               kHostAllocatedAddress, kHostAllocatedSize);
  int32_t dimension = op.getDimension();

  return ::tt::target::ttnn::CreateSoftmaxOp(*cache.fbb, in, out, dimension);
}

::flatbuffers::Offset<::tt::target::ttnn::DeallocateOp>
createDeallocateOp(FlatbufferObjectCache &cache, DeallocateOp op) {
  auto in =
      cache.at<::tt::target::TensorRef>(getOperandThroughDPSOps(op.getInput()));
  auto force = op.getForceAttr().getValue();
  return ::tt::target::ttnn::CreateDeallocateOp(*cache.fbb, in, force);
}

::flatbuffers::Offset<::tt::target::ttnn::Operation>
emitTTNNOperation(FlatbufferObjectCache &cache, Operation *op,
                  std::string const &debugString, std::string const &locInfo) {
  if (auto getDeviceOp = dyn_cast<GetDeviceOp>(op); getDeviceOp) {
    return createOperation(cache, createOp(cache, getDeviceOp), debugString,
                           locInfo);
  }
  if (auto toMemoryConfigOp = dyn_cast<ToMemoryConfigOp>(op);
      toMemoryConfigOp) {
    return createOperation(cache, createOp(cache, toMemoryConfigOp),
                           debugString, locInfo);
  }
  if (auto toLayoutOp = dyn_cast<ToLayoutOp>(op); toLayoutOp) {
    return createOperation(cache, createOp(cache, toLayoutOp), debugString,
                           locInfo);
  }
  if (auto toDTypeOp = dyn_cast<ToDTypeOp>(op); toDTypeOp) {
    return createOperation(cache, createOp(cache, toDTypeOp), debugString,
                           locInfo);
  }
  if (auto typecastOp = dyn_cast<TypecastOp>(op); typecastOp) {
    return createOperation(cache, createOp(cache, typecastOp), debugString,
                           locInfo);
  }
  if (auto toDeviceOp = dyn_cast<ToDeviceOp>(op); toDeviceOp) {
    return createOperation(cache, createOp(cache, toDeviceOp), debugString,
                           locInfo);
  }
  if (auto fromDeviceOp = dyn_cast<FromDeviceOp>(op); fromDeviceOp) {
    return createOperation(cache, createOp(cache, fromDeviceOp), debugString,
                           locInfo);
  }
  if (auto emptyOp = dyn_cast<EmptyOp>(op); emptyOp) {
    return createOperation(cache, createOp(cache, emptyOp), debugString,
                           locInfo);
  }
  if (auto fullOp = dyn_cast<FullOp>(op); fullOp) {
    return createOperation(cache, createOp(cache, fullOp), debugString,
                           locInfo);
  }
  if (auto arangeOp = dyn_cast<ArangeOp>(op); arangeOp) {
    return createOperation(cache, createOp(cache, arangeOp), debugString,
                           locInfo);
  }
  if (auto zerosOp = dyn_cast<ZerosOp>(op); zerosOp) {
    return createOperation(cache, createOp(cache, zerosOp), debugString,
                           locInfo);
  }
  if (auto onesOp = dyn_cast<OnesOp>(op); onesOp) {
    return createOperation(cache, createOp(cache, onesOp), debugString,
                           locInfo);
  }
  if (auto absOp = dyn_cast<AbsOp>(op); absOp) {
    return createOperation(cache, createEltwiseOp(cache, absOp), debugString,
                           locInfo);
  }
  if (auto addOp = dyn_cast<AddOp>(op); addOp) {
    return createOperation(cache, createEltwiseOp(cache, addOp), debugString,
                           locInfo);
  }
  if (auto floorOp = dyn_cast<FloorOp>(op); floorOp) {
    return createOperation(cache, createEltwiseOp(cache, floorOp), debugString,
                           locInfo);
  }
  if (auto isFiniteOp = dyn_cast<IsFiniteOp>(op); isFiniteOp) {
    return createOperation(cache, createEltwiseOp(cache, isFiniteOp),
                           debugString, locInfo);
  }
  if (auto cbrtOp = dyn_cast<CbrtOp>(op); cbrtOp) {
    return createOperation(cache, createEltwiseOp(cache, cbrtOp), debugString,
                           locInfo);
  }
  if (auto andOp = dyn_cast<LogicalAndOp>(op); andOp) {
    return createOperation(cache, createEltwiseOp(cache, andOp), debugString,
                           locInfo);
  }
  if (auto orOp = dyn_cast<LogicalOrOp>(op); orOp) {
    return createOperation(cache, createEltwiseOp(cache, orOp), debugString,
                           locInfo);
  }
  if (auto xorOp = dyn_cast<LogicalXorOp>(op); xorOp) {
    return createOperation(cache, createEltwiseOp(cache, xorOp), debugString,
                           locInfo);
  }
  if (auto notOp = dyn_cast<LogicalNotOp>(op); notOp) {
    return createOperation(cache, createEltwiseOp(cache, notOp), debugString,
                           locInfo);
  }
  if (auto bitwiseAndOp = dyn_cast<BitwiseAndOp>(op); bitwiseAndOp) {
    return createOperation(cache, createEltwiseOp(cache, bitwiseAndOp),
                           debugString, locInfo);
  }
  if (auto bitwiseOrOp = dyn_cast<BitwiseOrOp>(op); bitwiseOrOp) {
    return createOperation(cache, createEltwiseOp(cache, bitwiseOrOp),
                           debugString, locInfo);
  }
  if (auto bitwiseXorOp = dyn_cast<BitwiseXorOp>(op); bitwiseXorOp) {
    return createOperation(cache, createEltwiseOp(cache, bitwiseXorOp),
                           debugString, locInfo);
  }
  if (auto bitwiseNotOp = dyn_cast<BitwiseNotOp>(op); bitwiseNotOp) {
    return createOperation(cache, createEltwiseOp(cache, bitwiseNotOp),
                           debugString, locInfo);
  }
  if (auto multiplyOp = dyn_cast<MultiplyOp>(op); multiplyOp) {
    return createOperation(cache, createEltwiseOp(cache, multiplyOp),
                           debugString, locInfo);
  }
  if (auto negOp = dyn_cast<NegOp>(op); negOp) {
    return createOperation(cache, createEltwiseOp(cache, negOp), debugString,
                           locInfo);
  }
  if (auto subtractOp = dyn_cast<SubtractOp>(op); subtractOp) {
    return createOperation(cache, createEltwiseOp(cache, subtractOp),
                           debugString, locInfo);
  }
  if (auto eqOp = dyn_cast<EqualOp>(op); eqOp) {
    return createOperation(cache, createEltwiseOp(cache, eqOp), debugString,
                           locInfo);
  }
  if (auto neOp = dyn_cast<NotEqualOp>(op); neOp) {
    return createOperation(cache, createEltwiseOp(cache, neOp), debugString,
                           locInfo);
  }
  if (auto geOp = dyn_cast<GreaterEqualOp>(op); geOp) {
    return createOperation(cache, createEltwiseOp(cache, geOp), debugString,
                           locInfo);
  }
  if (auto gtOp = dyn_cast<GreaterThanOp>(op); gtOp) {
    return createOperation(cache, createEltwiseOp(cache, gtOp), debugString,
                           locInfo);
  }
  if (auto leOp = dyn_cast<LessEqualOp>(op); leOp) {
    return createOperation(cache, createEltwiseOp(cache, leOp), debugString,
                           locInfo);
  }
  if (auto ltOp = dyn_cast<LessThanOp>(op); ltOp) {
    return createOperation(cache, createEltwiseOp(cache, ltOp), debugString,
                           locInfo);
  }
  if (auto maximumOp = dyn_cast<MaximumOp>(op); maximumOp) {
    return createOperation(cache, createEltwiseOp(cache, maximumOp),
                           debugString, locInfo);
  }
  if (auto minimumOp = dyn_cast<MinimumOp>(op); minimumOp) {
    return createOperation(cache, createEltwiseOp(cache, minimumOp),
                           debugString, locInfo);
  }
  if (auto reluOp = dyn_cast<ReluOp>(op); reluOp) {
    return createOperation(cache, createEltwiseOp(cache, reluOp), debugString,
                           locInfo);
  }
  if (auto sqrtOp = dyn_cast<SqrtOp>(op); sqrtOp) {
    return createOperation(cache, createEltwiseOp(cache, sqrtOp), debugString,
                           locInfo);
  }
  if (auto rsqrtOp = dyn_cast<RsqrtOp>(op); rsqrtOp) {
    return createOperation(cache, createEltwiseOp(cache, rsqrtOp), debugString,
                           locInfo);
  }
  if (auto signOp = dyn_cast<SignOp>(op); signOp) {
    return createOperation(cache, createEltwiseOp(cache, signOp), debugString,
                           locInfo);
  }
  if (auto expOp = dyn_cast<ExpOp>(op); expOp) {
    return createOperation(cache, createEltwiseOp(cache, expOp), debugString,
                           locInfo);
  }
  if (auto logOp = dyn_cast<LogOp>(op); logOp) {
    return createOperation(cache, createEltwiseOp(cache, logOp), debugString,
                           locInfo);
  }
  if (auto expm1Op = dyn_cast<Expm1Op>(op); expm1Op) {
    return createOperation(cache, createEltwiseOp(cache, expm1Op), debugString,
                           locInfo);
  }
  if (auto sigmoidOp = dyn_cast<SigmoidOp>(op); sigmoidOp) {
    return createOperation(cache, createEltwiseOp(cache, sigmoidOp),
                           debugString, locInfo);
  }
  if (auto log1pOp = dyn_cast<Log1pOp>(op); log1pOp) {
    return createOperation(cache, createEltwiseOp(cache, log1pOp), debugString,
                           locInfo);
  }
  if (auto scatterOp = dyn_cast<ScatterOp>(op); scatterOp) {
    return createOperation(cache, createEltwiseOp(cache, scatterOp),
                           debugString, locInfo);
  }
  if (auto reciprocalOp = dyn_cast<ReciprocalOp>(op); reciprocalOp) {
    return createOperation(cache, createEltwiseOp(cache, reciprocalOp),
                           debugString, locInfo);
  }
  if (auto divOp = dyn_cast<DivOp>(op); divOp) {
    return createOperation(cache, createEltwiseOp(cache, divOp), debugString,
                           locInfo);
  }
  if (auto remainderOp = dyn_cast<RemainderOp>(op); remainderOp) {
    return createOperation(cache, createEltwiseOp(cache, remainderOp),
                           debugString, locInfo);
  }
  if (auto leakyReluOp = dyn_cast<LeakyReluOp>(op); leakyReluOp) {
    return createOperation(cache, createEltwiseOp(cache, leakyReluOp),
                           debugString, locInfo);
  }
  if (auto powerOp = dyn_cast<PowerOp>(op); powerOp) {
    return createOperation(cache, createEltwiseOp(cache, powerOp), debugString,
                           locInfo);
  }
  if (auto linearOp = dyn_cast<LinearOp>(op); linearOp) {
    return createOperation(cache, createOp(cache, linearOp), debugString,
                           locInfo);
  }
  if (auto matmulOp = dyn_cast<MatmulOp>(op); matmulOp) {
    return createOperation(cache, createOp(cache, matmulOp), debugString,
                           locInfo);
  }
  if (auto morehCumSumOp = dyn_cast<MorehCumSumOp>(op); morehCumSumOp) {
    return createOperation(cache, createOp(cache, morehCumSumOp), debugString,
                           locInfo);
  }
  if (auto sumOp = dyn_cast<SumOp>(op); sumOp) {
    return createOperation(cache, createReductionOp(cache, sumOp), debugString,
                           locInfo);
  }
  if (auto meanOp = dyn_cast<MeanOp>(op); meanOp) {
    return createOperation(cache, createReductionOp(cache, meanOp), debugString,
                           locInfo);
  }
  if (auto maxOp = dyn_cast<MaxOp>(op); maxOp) {
    return createOperation(cache, createReductionOp(cache, maxOp), debugString,
                           locInfo);
  }
  if (auto minOp = dyn_cast<MinOp>(op); minOp) {
    return createOperation(cache, createReductionOp(cache, minOp), debugString,
                           locInfo);
  }
  if (auto prodOp = dyn_cast<ProdOp>(op); prodOp) {
    return createOperation(cache, createReductionProdOp(cache, prodOp),
                           debugString, locInfo);
  }
  if (auto embeddingOp = dyn_cast<EmbeddingOp>(op); embeddingOp) {
    return createOperation(cache, createEmbeddingOp(cache, embeddingOp),
                           debugString, locInfo);
  }
  if (auto embeddingBackwardOp = dyn_cast<EmbeddingBackwardOp>(op);
      embeddingBackwardOp) {
    return createOperation(
        cache, createEmbeddingBackwardOp(cache, embeddingBackwardOp),
        debugString, locInfo);
  }
  if (auto repeatInterleaveOp = dyn_cast<RepeatInterleaveOp>(op);
      repeatInterleaveOp) {
    return createOperation(cache,
                           createRepeatInterleaveOp(cache, repeatInterleaveOp),
                           debugString, locInfo);
  }
  if (auto softmaxOp = dyn_cast<SoftmaxOp>(op); softmaxOp) {
    return createOperation(cache, createSoftmaxOp(cache, softmaxOp),
                           debugString, locInfo);
  }
  if (auto transposeOp = dyn_cast<TransposeOp>(op); transposeOp) {
    return createOperation(cache, createTransposeOp(cache, transposeOp),
                           debugString, locInfo);
  }
  if (auto clampOp = dyn_cast<ClampOp>(op); clampOp) {
    return createOperation(cache, createNonDPSEltwiseOp(cache, clampOp),
                           debugString, locInfo);
  }
  if (auto conv2dOp = dyn_cast<Conv2dOp>(op); conv2dOp) {
    return createOperation(cache, createOp(cache, conv2dOp), debugString,
                           locInfo);
  }
  if (auto conv_transpose2dOp = dyn_cast<ConvTranspose2dOp>(op);
      conv_transpose2dOp) {
    return createOperation(cache, createOp(cache, conv_transpose2dOp),
                           debugString, locInfo);
  }
  if (auto allGatherOp = dyn_cast<AllGatherOp>(op); allGatherOp) {
    return createOperation(cache, createOp(cache, allGatherOp), debugString,
                           locInfo);
  }
  if (auto reduceScatterOp = dyn_cast<ReduceScatterOp>(op); reduceScatterOp) {
    return createOperation(cache, createOp(cache, reduceScatterOp), debugString,
                           locInfo);
  }
  if (auto meshShardOp = dyn_cast<MeshShardOp>(op); meshShardOp) {
    return createOperation(cache, createOp(cache, meshShardOp), debugString,
                           locInfo);
  }
  if (auto concatOp = dyn_cast<ConcatOp>(op); concatOp) {
    return createOperation(cache, createConcatOp(cache, concatOp), debugString,
                           locInfo);
  }
  if (auto reshapeOp = dyn_cast<ReshapeOp>(op); reshapeOp) {
    return createOperation(cache, createReshapeOp(cache, reshapeOp),
                           debugString, locInfo);
  }
  if (auto repeatOp = dyn_cast<RepeatOp>(op); repeatOp) {
    return createOperation(cache, createRepeatOp(cache, repeatOp), debugString,
                           locInfo);
  }
  if (auto padOp = dyn_cast<PadOp>(op); padOp) {
    return createOperation(cache, createPadOp(cache, padOp), debugString,
                           locInfo);
  }
  if (auto sliceOp = dyn_cast<SliceOp>(op); sliceOp) {
    return createOperation(cache, createSliceOp(cache, sliceOp), debugString,
                           locInfo);
  }
  if (auto max_pool2dOp = dyn_cast<MaxPool2dOp>(op); max_pool2dOp) {
    return createOperation(cache, createMaxPool2dOp(cache, max_pool2dOp),
                           debugString, locInfo);
  }
  if (auto deallocateOp = dyn_cast<DeallocateOp>(op); deallocateOp) {
    return createOperation(cache, createDeallocateOp(cache, deallocateOp),
                           debugString, locInfo);
  }
  if (auto ceilOp = dyn_cast<CeilOp>(op); ceilOp) {
    return createOperation(cache, createEltwiseOp(cache, ceilOp), debugString,
                           locInfo);
  }
  if (auto cosOp = dyn_cast<CosOp>(op); cosOp) {
    return createOperation(cache, createEltwiseOp(cache, cosOp), debugString,
                           locInfo);
  }
  if (auto sinOp = dyn_cast<SinOp>(op); sinOp) {
    return createOperation(cache, createEltwiseOp(cache, sinOp), debugString,
                           locInfo);
  }
  if (auto whereOp = dyn_cast<WhereOp>(op); whereOp) {
    return createOperation(cache, createEltwiseOp(cache, whereOp), debugString,
                           locInfo);
  }
  if (auto geluOp = dyn_cast<GeluOp>(op); geluOp) {
    return createOperation(cache, createEltwiseOp(cache, geluOp), debugString,
                           locInfo);
  }
  if (auto tanOp = dyn_cast<TanOp>(op); tanOp) {
    return createOperation(cache, createEltwiseOp(cache, tanOp), debugString,
                           locInfo);
  }
  if (auto tanhOp = dyn_cast<TanhOp>(op); tanhOp) {
    return createOperation(cache, createEltwiseOp(cache, tanhOp), debugString,
                           locInfo);
  }
  if (auto updateCacheOp = dyn_cast<UpdateCacheOp>(op); updateCacheOp) {
    return createOperation(cache, createOp(cache, updateCacheOp), debugString,
                           locInfo);
  }
  if (auto fillCacheOp = dyn_cast<FillCacheOp>(op); fillCacheOp) {
    return createOperation(cache, createOp(cache, fillCacheOp), debugString,
                           locInfo);
  }
  if (auto permuteOp = dyn_cast<PermuteOp>(op); permuteOp) {
    return createOperation(cache, createOp(cache, permuteOp), debugString,
                           locInfo);
  }
  if (auto upsampleOp = dyn_cast<UpsampleOp>(op); upsampleOp) {
    return createOperation(cache, createOp(cache, upsampleOp), debugString,
                           locInfo);
  }

  llvm_unreachable("unhandled op in emitTTNNOperation");
}

std::shared_ptr<void> ttnnToFlatbuffer(
    Operation *op,
    const std::unordered_map<std::string, GoldenTensor> &goldenMap,
    const std::vector<std::pair<std::string, std::string>> &moduleCache) {
  ModuleOp module = dyn_cast<ModuleOp>(op);
  assert(module && "Expected ModuleOp as top level operation");

  ::flatbuffers::FlatBufferBuilder fbb;
  FlatbufferObjectCache cache(&fbb);

  ::ttmlir::Version ttmlirVersion = ::ttmlir::getVersion();
  ::tt::target::Version binaryVersion(ttmlirVersion.major, ttmlirVersion.minor,
                                      ttmlirVersion.patch);

  auto systemDesc =
      toFlatbuffer(cache, mlir::cast<tt::SystemDescAttr>(
                              module->getAttr(tt::SystemDescAttr::name)));

  auto mlir = toDebugInfo(fbb, "ttnn", module);
  std::string cpp;
  llvm::raw_string_ostream os(cpp);
  auto result = mlir::tt::ttnn::emitTTNNAsCpp(module, os);
  (void)result;

  std::vector<::flatbuffers::Offset<::tt::target::GoldenKV>> goldenKVList;
  goldenKVList.reserve(goldenMap.size());

  for (auto element : goldenMap) {
    std::vector<std::uint8_t> dataTensor = element.second.convertDataToVector();
    auto goldenTensor = ::tt::target::CreateGoldenTensorDirect(
        fbb, element.second.name.c_str(), &element.second.shape,
        &element.second.strides, element.second.dtype, &dataTensor);
    auto goldenKV = ::tt::target::CreateGoldenKVDirect(
        fbb, element.first.c_str(), goldenTensor);
    goldenKVList.push_back(goldenKV);
  }

  // Load the ModuleCache if present and populate DebugInfo
  std::vector<::flatbuffers::Offset<::tt::target::MLIR>> moduleCacheList;
  moduleCacheList.reserve(moduleCache.size());

  for (const auto &item : moduleCache) {
    // Here the Name is the Pass Name and Source is the IR itself
    auto moduleCacheItem = ::tt::target::CreateMLIRDirect(
        fbb, item.first.c_str(), item.second.c_str());
    moduleCacheList.push_back(moduleCacheItem);
  }

  auto goldenInfo = ::tt::target::CreateGoldenInfoDirect(fbb, &goldenKVList);
  auto debugInfo = ::tt::target::CreateDebugInfoDirect(
      fbb, mlir, cpp.c_str(), &moduleCacheList, goldenInfo);

  std::vector<::flatbuffers::Offset<::tt::target::ttnn::Program>> programs;
  module->walk([&](func::FuncOp func) {
    Program<::tt::target::ttnn::Operation> program =
        funcOpToProgram<::tt::target::ttnn::Operation>(cache, func,
                                                       emitTTNNOperation);
    programs.push_back(::tt::target::ttnn::CreateProgramDirect(
        fbb, program.name, &program.inputs, &program.outputs, &program.ops,
        debugInfo));
  });

  auto binary = ::tt::target::ttnn::CreateTTNNBinaryDirect(
      fbb, &binaryVersion, ::ttmlir::getGitHash(), systemDesc, &programs);

  ::tt::target::ttnn::FinishSizePrefixedTTNNBinaryBuffer(fbb, binary);
  ::flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  ::tt::target::ttnn::VerifySizePrefixedTTNNBinaryBuffer(verifier);

  uint8_t *buf = fbb.GetBufferPointer();
  std::size_t size = fbb.GetSize();

  std::shared_ptr<void> bufferPtr =
      std::shared_ptr<void>(std::malloc(size), std::free);
  std::memcpy(bufferPtr.get(), buf, size);
  return bufferPtr;
}

LogicalResult translateTTNNToFlatbuffer(
    Operation *op, llvm::raw_ostream &os,
    const std::unordered_map<std::string, GoldenTensor> &goldenMap,
    const std::vector<std::pair<std::string, std::string>> &moduleCache) {
  std::shared_ptr<void> data = ttnnToFlatbuffer(op, goldenMap, moduleCache);
  std::size_t size = ::flatbuffers::GetSizePrefixedBufferLength(
      static_cast<const uint8_t *>(data.get()));
  os.write(reinterpret_cast<char const *>(data.get()), size);
  return success();
}
} // namespace mlir::tt::ttnn
