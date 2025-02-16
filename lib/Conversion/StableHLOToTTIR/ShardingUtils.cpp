// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ttmlir/Conversion/StableHLOToTTIR/ShardingUtils.h"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallVector.h"

#include <numeric>

namespace mlir {
namespace tt {
namespace sharding_utils {

// Based on current MeshSharding info, finalize sharding dimensions.
static LogicalResult
determineGSPMDShardingDims(MeshSharding &meshSharding,
                           const bool lastTileDimReplicate) {
  // This code is based on following assumption.
  // 1. Hardware mesh is two dimenion such as 2x4, 1x2, ...
  // 2. Hardware mesh only supports either line or mesh config
  // e.g., t3k 1x8 or 2x4
  SmallVector<int64_t> shardShape = meshSharding.shardShape;
  if (meshSharding.lastTileDimReplicate) {
    meshSharding.shardShape.pop_back();
  }
  // Determine obvious properties first.
  bool reverseOrder = meshSharding.meshShape.size() != 1;
  // totalDevices is the total number of multi-chips such as 8 for t3k. Thus, no
  // overflow is expected with int64_t.
  int64_t totalDevices = std::accumulate(
      meshSharding.meshShape.begin(), meshSharding.meshShape.end(), int64_t{1},
      std::multiplies<int64_t>());
  // Detect line device config (1xN).
  bool isLineDeviceConfig =
      llvm::any_of(shardShape, [&](int64_t s) { return s == totalDevices; });
  // Detect hardware mesh. For reverse order sharding, meshShape already
  // includes hardware mesh. For non reverse order case, extract hardware mesh
  // by traversing from front to back and picking none-zero values.
  if (!reverseOrder) {
    if (isLineDeviceConfig) {
      // Device with line config must be 1xN, not Nx1.
      meshSharding.meshShape = {1, meshSharding.meshShape[0]};
    } else {
      // e.g., shardShape [1,2,4] or [2,1,4] leads to [2,4]
      SmallVector<int64_t> meshShape(shardShape.size(), 0);
      auto *it = llvm::copy_if(shardShape, meshShape.begin(),
                               [](int64_t s) { return !(s == int64_t{1}); });
      meshShape.resize(std::distance(meshShape.begin(), it));
      meshSharding.meshShape = meshShape;
    }
  }

  if (meshSharding.meshShape.size() != 2) {
    // Currently, we are only supporting 2d hardware mesh config.
    return failure();
  }

  // Determine shardDims based on the shardShape and meshShape.
  // shard_dims indicate in which dimension we shard the tensor. For T3K,
  // detected meshShape will be [2, 4] and shard_dims will be [ a, b ] depending
  // on the sharding intention.
  // For example, if shardShape is [1,2,1,4], shard_dims is supposed to be [1,
  // 3] or if shardShape is [1,4,1,2], then shard_dims should be [3, 1].
  meshSharding.shardDims.assign(meshSharding.meshShape.size(), -1);
  // Skip the first 1 of 1xN hardware.
  uint64_t shardingCnt = isLineDeviceConfig;
  for (uint64_t i = 0; i < meshSharding.shardShape.size(); ++i) {
    // Check sharding dimension only.
    if (meshSharding.shardShape[i] != 1) {
      auto shardDimIdx = (reverseOrder)
                             ? (meshSharding.meshShape.size() - 1 - shardingCnt)
                             : shardingCnt;
      if (meshSharding.shardShape[i] != meshSharding.meshShape[shardDimIdx]) {
        // shardShape[i] and meshShape[shardDimIdx] is supposed to be identical.
        return failure();
      }
      meshSharding.shardDims[shardDimIdx] = i;
      shardingCnt++;
    }
  }

  return success();
}

// Parse GSPMD devices string and fill out MeshSharding info.
static LogicalResult parseGSPMDDevicesStr(const StringRef devicesStr,
                                          MeshSharding &meshSharding) {
  // This function extract dimensions from targetDimsStr "[x,y,z]" and saves it
  // to targetDims.
  auto parseDimsFromDimensionStr =
      [](StringRef targetDimsStr,
         SmallVector<int64_t> &targetDims) -> LogicalResult {
    if (!targetDimsStr.consume_front("[") || !targetDimsStr.consume_back("]")) {
      return failure();
    }
    SmallVector<StringRef> dimsStr;
    targetDimsStr.split(dimsStr, ",");
    for (auto dim : dimsStr) {
      int64_t d;
      if (dim.getAsInteger<int64_t>(10, d)) {
        return failure();
      }
      targetDims.push_back(d);
    }
    return success();
  };

  // devicesStr is generated by splitting whole string using space " ". Thus, it
  // is not supposed to include any trailing space. e.g., "[4,2,1]<=[2,4]T(1,0)"
  auto [axesStr, restStr] = devicesStr.split("<=");
  // Parse devices string before "<=" e.g., [4,2,1].
  if (failed(parseDimsFromDimensionStr(axesStr, meshSharding.shardShape))) {
    return failure();
  }
  // Parse devices string after "<=" e.g., [8] or [2,4]T(1,0).
  SmallVector<StringRef> reshapeStr;
  restStr.split(reshapeStr, "T");
  // Parse reshape[0] string e.g., [8] or [2,4].
  if (failed(
          parseDimsFromDimensionStr(reshapeStr[0], meshSharding.meshShape))) {
    return failure();
  }
  return success();
}

// OpenXLA has its own lexer, but we will use simple string-based parser here.
// This parsing is mainly based on "Sharding Attribute" section in
// https://github.com/sdasgup3/stablehlo/blob/80082431d1af0933e6202ecc8a6f8801e039235b/docs/spec.md#sharding-attribute
LogicalResult parseGSPMDShardingAttr(StringRef shardingStr,
                                     MeshSharding &meshSharding) {
  MeshShardType shardType = mlir::tt::MeshShardType::Manual;
  bool lastTileDimReplicate = false;

  // Parse sting and tokenize.
  if (!shardingStr.consume_front("{") || !shardingStr.consume_back("}")) {
    return failure();
  }
  SmallVector<StringRef> shardingStrTokens;
  shardingStr.split(shardingStrTokens, " ");

  // Parse tokens.
  for (auto str : shardingStrTokens) {
    if (str.contains("manual")) {
      assert(shardType == mlir::tt::MeshShardType::Manual &&
             "Fail to parse sharding info.");
      // manual: already sharded, so no action is needed
      meshSharding.shardShape.push_back(1);
    } else if (str.contains("replicated")) {
      assert(shardType == mlir::tt::MeshShardType::Manual &&
             "Fail to parse sharding info.");
      // replicated: all devices have whole data
      shardType = mlir::tt::MeshShardType::Replicate;
      meshSharding.shardShape.push_back(1);
    } else if (str.contains("maximal")) {
      assert(shardType == mlir::tt::MeshShardType::Manual &&
             "Fail to parse sharding info.");
      // maximal: one device has whole data
      shardType = mlir::tt::MeshShardType::Maximal;
      meshSharding.shardShape.push_back(1);
    } else if (str.consume_front("device=")) {
      // maximal should followed by "device" to put data on
      assert(shardType == mlir::tt::MeshShardType::Maximal &&
             "Fail to parse sharding info.");
      int64_t d;
      if (str.getAsInteger<int64_t>(10, d)) {
        return failure();
      }
      meshSharding.shardShape.push_back(d);
    } else if (str.consume_front("devices=")) {
      // other: "devices" detail sharding plan
      assert(shardType == mlir::tt::MeshShardType::Manual &&
             "Fail to parse sharding info.");
      shardType = mlir::tt::MeshShardType::Devices;
      if (failed(parseGSPMDDevicesStr(str, meshSharding))) {
        return failure();
      }
    } else if (str.contains("last_tile_dim_replicate")) {
      assert(shardType == mlir::tt::MeshShardType::Devices &&
             "Fail to parse sharding info.");
      // other: replicate last tile dim
      lastTileDimReplicate = true;
    }
  }
  meshSharding.shardType = shardType;
  meshSharding.lastTileDimReplicate = lastTileDimReplicate;

  // Parse devices
  if (meshSharding.shardType == mlir::tt::MeshShardType::Devices) {
    return determineGSPMDShardingDims(meshSharding, lastTileDimReplicate);
  }

  meshSharding.shardDims.push_back(-1);
  meshSharding.meshShape.push_back(-1);
  return success();
}

} // namespace sharding_utils
} // namespace tt
} // namespace mlir
