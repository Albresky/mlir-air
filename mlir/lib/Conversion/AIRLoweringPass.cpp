// (c) Copyright 2020 Xilinx Inc. All Rights Reserved.


#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/Affine/EDSC/Builders.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/StandardOps/EDSC/Builders.h"
#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/MemRef/EDSC/Intrinsics.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/SCF/EDSC/Builders.h"
#include "mlir/EDSC/Builders.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include <vector>

#include "PassDetail.h"

#include "air/Dialect/AIR/AIRDialect.h"
#include "air/Dialect/AIRRt/AIRRtDialect.h"
#include "air/Dialect/AIRRt/AIRRtOps.h"
#include "air/Util/Util.h"

#define DEBUG_TYPE "air-lowering-pass"

using namespace mlir;
using namespace edsc::intrinsics;

using callOperation = edsc::OperationBuilder<mlir::CallOp>;
using call = edsc::ValueBuilder<mlir::CallOp>;
using constInt = edsc::intrinsics::std_constant_int;
using constFloat = edsc::intrinsics::std_constant_float;

namespace {

#ifdef XTEN_DIALECT

Value typeCast(PatternRewriter &builder, Value val, Type destTy) {
  if (val.getType() == destTy)
    return val;
  return builder.create<NPCOMP::aten::TypeCastOp>(val.getLoc(), destTy, val)
      .getResult();
}

/// Create a type cast to memref
Value MemRefTypeCast(PatternRewriter &builder, Value val) {
  if (val.getType().isa<MemRefType>())
    return val;
  auto tensorTy = val.getType().dyn_cast<TensorType>();
  if (!tensorTy)
    return val;
  auto memRefType = mlir::MemRefType::get(tensorTy.getShape(), tensorTy.getElementType(), {}, 0);
  return typeCast(builder, val, memRefType);
}

class NoOpConversion_affine : public ConversionPattern {
public:
  explicit NoOpConversion_affine(MLIRContext *context)
      : ConversionPattern(xilinx::xten::NoOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto noop = cast<xilinx::xten::NoOp>(op);
    auto loc = noop.getLoc();
    Type resultTy = noop.getResult().getType();
    TensorType tensorResultTy = resultTy.cast<TensorType>();
    MemRefType memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                      tensorResultTy.getElementType(),
                                                      {}, 0);

    Value result = rewriter.create<memref::AllocOp>(loc, memRefResultTy);
    Value lhs = MemRefTypeCast(rewriter, operands[0]);

    using namespace edsc;

    ScopedContext scope(rewriter, loc);
    Value zero = intrinsics::std_constant_index(0);
    MemRefBoundsCapture vRes(result);
    MemRefIndexedValue iRes(result), iLHS(lhs);
    Value M(vRes.ub(0));
    if (vRes.rank() == 1) {
      affineLoopNestBuilder({zero}, {M},
                            1, [&] (ValueRange ivs) {
                              Value i = ivs[0];
                              iRes(i) = iLHS(i);
                            });
    } else if (vRes.rank() == 2) {
      Value N(vRes.ub(1));
      affineLoopNestBuilder({zero, zero}, {M, N},
                            {1,1}, [&] (ValueRange ivs) {
                              Value i = ivs[0]; Value j = ivs[1];
                              iRes(i, j) = iLHS(i, j);
                            });
    } else if (vRes.rank() == 3) {
      Value N(vRes.ub(1));
      Value O(vRes.ub(2));
      affineLoopNestBuilder({zero, zero, zero}, {M, N, O},
                            {1,1,1}, [&](ValueRange ivs) {
                              Value i = ivs[0]; Value j = ivs[1]; Value k = ivs[2];
                              iRes(i, j, k) = iLHS(i, j, k);
                            });
    }
    else {
      Value N(vRes.ub(1));
      Value O(vRes.ub(2));
      Value P(vRes.ub(3));
      affineLoopNestBuilder({zero, zero, zero, zero},
                            {M, N, O, P},
                            {1,1,1,1}, [&](ValueRange ivs) {
                              Value i = ivs[0]; Value j = ivs[1]; Value k = ivs[2]; Value l = ivs[3];
                              iRes(i, j, k, l) = iLHS(i, j, k, l);
                            });
    }

    auto tensor_cast = rewriter.create<NPCOMP::aten::TypeCastOp>(loc, tensorResultTy, result).getResult();
    rewriter.replaceOp(op, {tensor_cast});
    return success();
  }
};

class NoOpConversion : public ConversionPattern {
public:
  explicit NoOpConversion(MLIRContext *context)
      : ConversionPattern(xilinx::xten::NoOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, {operands[0]});
    return success();
  }
};

/ Lower conv2d
class AIRConv2dConversion : public ConversionPattern {
public:
  explicit AIRConv2dConversion(MLIRContext *context)
      : ConversionPattern(xilinx::xten::Conv2dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    Type resultTy = op->getResult(0).getType();
    TensorType tensorResultTy = resultTy.cast<TensorType>();
    Type memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto loc = op->getLoc();
    edsc::ScopedContext scope(rewriter, loc);

    Value xVal(MemRefTypeCast(rewriter, operands[0]));
    Value wVal(MemRefTypeCast(rewriter, operands[1]));
    Value bVal(MemRefTypeCast(rewriter, operands[2]));

    std::vector<int64_t> pad, kernel, stride;
    unpack_int_list(operands[3], pad);
    unpack_int_list(operands[4], kernel);
    unpack_int_list(operands[5], stride);

    auto padCI = constInt(pad[0],32);
    auto kernelCI = constInt(kernel[0], 32);
    auto strideCI = constInt(stride[0], 32);

    std::vector<Value> callops{xVal, wVal, bVal, padCI, kernelCI, strideCI};

    FuncOp convFunc = xilinx::air::getATenFn(op->getParentOfType<ModuleOp>(),
                                "conv2d", callops, memRefResultTy);

    auto new_call = callOperation(memRefResultTy,
                                  rewriter.getSymbolRefAttr(convFunc),
                                  callops);

    auto tensor_cast = rewriter.create<NPCOMP::aten::TypeCastOp>(loc, tensorResultTy, (*new_call).getResults()).getResult();
    rewriter.replaceOp(op, {tensor_cast});
    return success();
  }
};

class AIRConv2dReLUConversion : public ConversionPattern {
public:
  explicit AIRConv2dReLUConversion(MLIRContext *context)
      : ConversionPattern(xilinx::xten::Conv2dReLUOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    Type resultTy = op->getResult(0).getType();
    TensorType tensorResultTy = resultTy.cast<TensorType>();
    Type memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto loc = op->getLoc();
    edsc::ScopedContext scope(rewriter, loc);

    Value xVal(MemRefTypeCast(rewriter, operands[0]));
    Value wVal(MemRefTypeCast(rewriter, operands[1]));
    Value bVal(MemRefTypeCast(rewriter, operands[2]));

    std::vector<int64_t> pad, kernel, stride;
    unpack_int_list(operands[3], pad);
    unpack_int_list(operands[4], kernel);
    unpack_int_list(operands[5], stride);

    auto padCI = constInt(pad[0],32);
    auto kernelCI = constInt(kernel[0], 32);
    auto strideCI = constInt(stride[0], 32);

    std::vector<Value> callops{xVal, wVal, bVal, padCI, kernelCI, strideCI};

    FuncOp convFunc = xilinx::air::getATenFn(op->getParentOfType<ModuleOp>(),
                                "conv2d_relu", callops, memRefResultTy);

    auto new_call = callOperation(memRefResultTy,
                                  rewriter.getSymbolRefAttr(convFunc),
                                  callops);

    auto tensor_cast = rewriter.create<NPCOMP::aten::TypeCastOp>(loc, tensorResultTy, (*new_call).getResults()).getResult();
    rewriter.replaceOp(op, {tensor_cast});
    return success();
  }
};

/ Lower conv2d
class AIRConv2dBatchNormReLUConversion : public ConversionPattern {
public:
  explicit AIRConv2dBatchNormReLUConversion(MLIRContext *context)
      : ConversionPattern(xilinx::xten::Conv2dBatchNormReLUOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    Type resultTy = op->getResult(0).getType();
    TensorType tensorResultTy = resultTy.cast<TensorType>();
    Type memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto loc = op->getLoc();
    edsc::ScopedContext scope(rewriter, loc);

    std::vector<Value> callops;
    // conv2d operands
    {
      Value xVal(MemRefTypeCast(rewriter, operands[0]));
      Value wVal(MemRefTypeCast(rewriter, operands[1]));
      Value bVal(MemRefTypeCast(rewriter, operands[2]));

      std::vector<int64_t> pad, kernel, stride;
      unpack_int_list(operands[3], pad);
      unpack_int_list(operands[4], kernel);
      unpack_int_list(operands[5], stride);

      auto padCI = constInt(pad[0],32);
      auto kernelCI = constInt(kernel[0], 32);
      auto strideCI = constInt(stride[0], 32);
      std::vector<Value> cops{xVal, wVal, bVal, padCI, kernelCI, strideCI};
      for (auto o : cops) callops.push_back(o);
    }
    {
      Value bVal(operands[11+1]);
      Value cVal(operands[11+2]);
      Value dVal(operands[11+3]);
      Value eVal(operands[11+4]);

      auto co0 = cast<NPCOMP::aten::ConstantOp>(operands[11+5].getDefiningOp());
      auto ia0 = co0->getAttrOfType<IntegerAttr>("value");
      APInt iaVal0 = ia0.getValue();

      auto co1 = cast<NPCOMP::aten::ConstantOp>(operands[11+6].getDefiningOp());
      auto fa0 = co1->getAttrOfType<FloatAttr>("value");
      APFloat faVal0 = fa0.getValue();

      auto co2 = cast<NPCOMP::aten::ConstantOp>(operands[11+7].getDefiningOp());
      auto fa1 = co2->getAttrOfType<FloatAttr>("value");
      APFloat faVal1 = fa1.getValue();

      auto f32Ty = FloatType::getF32(op->getContext());

      std::vector<Value> cops{bVal, cVal, dVal, eVal,
                              constInt(iaVal0.getZExtValue(), 1),
                              constFloat(faVal0, f32Ty),
                              constFloat(faVal1, f32Ty)};
      for (auto o : cops) callops.push_back(o);
    }
    FuncOp convFunc = xilinx::air::getATenFn(op->getParentOfType<ModuleOp>(),
                                "conv2d_bn_relu", callops, memRefResultTy);

    auto new_call = callOperation(memRefResultTy,
                                  rewriter.getSymbolRefAttr(convFunc),
                                  callops);

    auto tensor_cast = rewriter.create<NPCOMP::aten::TypeCastOp>(loc, tensorResultTy, (*new_call).getResults()).getResult();
    rewriter.replaceOp(op, {tensor_cast});
    return success();
  }
};
#endif //XTEN_DIALECT

class AIRHerdLaunchConversion : public ConversionPattern {
public:
  explicit AIRHerdLaunchConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::HerdLaunchOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    xilinx::air::HerdLaunchOp launch = cast<xilinx::air::HerdLaunchOp>(op);
    if (auto attr = op->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName())) {
      auto herd_name = attr.getValue().str();
      SmallVector<Value, 1> args;
      rewriter.create<xilinx::airrt::HerdLoadOp>(op->getLoc(), rewriter.getI32Type(), herd_name, args);
    }
    auto herd_size = launch.getHerdSizeOperands();
    int64_t herd_size_x = cast<ConstantIndexOp>(herd_size.x.getDefiningOp()).getValue();
    int64_t herd_size_y = cast<ConstantIndexOp>(herd_size.y.getDefiningOp()).getValue();

    auto outer = rewriter.create<AffineForOp>(launch.getLoc(), 0, herd_size_x);
    auto outer_builder = OpBuilder::atBlockBegin(outer.getBody());
    auto inner = outer_builder.create<AffineForOp>(launch.getLoc(), 0, herd_size_y);

    outer->setAttr("air.herd_launch", StringAttr::get(op->getContext(), "outer"));
    inner->setAttr("air.herd_launch", StringAttr::get(op->getContext(), "inner"));

    launch.getHerdSize().x.replaceAllUsesWith(herd_size.x);
    launch.getHerdSize().y.replaceAllUsesWith(herd_size.y);
    launch.getTileIds().x.replaceAllUsesWith(outer.getInductionVar());
    launch.getTileIds().y.replaceAllUsesWith(inner.getInductionVar());
    
    int i=0;
    for (auto arg : launch.getKernelArguments())
      arg.replaceAllUsesWith(launch.getKernelOperand(i++));

    auto &body = launch.body().front().getOperations();
    inner.getBody()->getOperations().splice(inner.getBody()->begin(), body,
                                            body.begin(), --body.end());
    rewriter.eraseOp(op);
    return success();
  }

};

static
CallOp convertDmaMemcpyToMemcpyFn(Operation *op, ArrayRef<Value > operands,
                                  ConversionPatternRewriter &rewriter, StringRef fnName) {
  auto loc = op->getLoc();

  SmallVector<Value, 16> callops;
  SmallVector<Type, 1> retTys{};

  auto idTy = IntegerType::get(op->getContext(), 32);
  if (auto id_attr = op->getAttrOfType<IntegerAttr>("id")) {
    callops.push_back(rewriter.create<ConstantOp>(loc, idTy, id_attr));
  } else {
    callops.push_back(rewriter.create<ConstantOp>(loc, idTy,
                                                  IntegerAttr::get(idTy, 0)));
  }

  xilinx::air::HerdLaunchOp launch = op->getParentOfType<xilinx::air::HerdLaunchOp>();
  if (!launch) {
    AffineForOp afo = op->getParentOfType<AffineForOp>();
    while (afo && !afo->getAttr("air.herd_launch"))
      afo = afo->getParentOfType<AffineForOp>();
    if (!afo) return nullptr;
    callops.push_back(afo.getInductionVar());

    afo = afo->getParentOfType<AffineForOp>();
    while (afo && !afo->getAttr("air.herd_launch"))
      afo = afo->getParentOfType<AffineForOp>();
    if (!afo) return nullptr;
    callops.push_back(afo.getInductionVar());
  }
  else {
    auto tileIds = launch.getTileIds();
    callops.push_back(tileIds.x);
    callops.push_back(tileIds.y);
  }

  for (auto o : operands)
    callops.push_back(o);

  SmallVector<Type, 16> tys;
  for (auto o : callops)
    tys.push_back(o.getType());

  auto fn = xilinx::air::getATenFn(op->getParentOfType<ModuleOp>(),
                                   fnName.str(), callops, {});
  auto call = rewriter.create<CallOp>(loc, retTys, rewriter.getSymbolRefAttr(fn), callops);
  rewriter.eraseOp(op);
  return call;
}

static
Operation* convertDmaMemcpyToAirRt(Operation *op, ArrayRef<Value > operands,
                                   ConversionPatternRewriter &rewriter) {
  auto dmaif = mlir::dyn_cast<xilinx::air::DmaMemcpyInterface>(op);
  auto loc = op->getLoc();

  MemRefType src = dmaif.getSrcMemref().getType().cast<MemRefType>();
  MemRefType dst = dmaif.getDstMemref().getType().cast<MemRefType>();
  bool isFromTile;
  if (src.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L1 &&
      dst.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L3) {
    isFromTile = true;
  }
  else if (dst.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L1 &&
           src.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L3) {
    isFromTile = false;
  }
  else if (src.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L1 &&
           dst.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L2) {
    isFromTile = true;
  }
  else if (dst.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L1 &&
           src.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L2) {
    isFromTile = false;
  }
  else
    return nullptr;

  SmallVector<Value, 16> opers;

  auto idTy = IntegerType::get(op->getContext(), 32);
  if (auto id_attr = op->getAttrOfType<IntegerAttr>("id")) {
    opers.push_back(rewriter.create<ConstantOp>(loc, idTy, id_attr));
  } else {
    opers.push_back(rewriter.create<ConstantOp>(loc, idTy,
                                                IntegerAttr::get(idTy, 0)));
  }

  xilinx::air::HerdLaunchOp launch = op->getParentOfType<xilinx::air::HerdLaunchOp>();
  if (!launch) {

    AffineForOp afo = op->getParentOfType<AffineForOp>();
    while (afo && !afo->getAttr("air.herd_launch"))
      afo = afo->getParentOfType<AffineForOp>();
    if (!afo) return nullptr;
    opers.push_back(afo.getInductionVar());

    afo = afo->getParentOfType<AffineForOp>();
    while (afo && !afo->getAttr("air.herd_launch"))
      afo = afo->getParentOfType<AffineForOp>();
    if (!afo) return nullptr;
    opers.push_back(afo.getInductionVar());
  }
  else {
    auto tileIds = launch.getTileIds();
    opers.push_back(tileIds.x);
    opers.push_back(tileIds.y);
  }

  for (auto o : operands)
      opers.push_back(o);

  if (isFromTile) {
    opers.erase(opers.begin() + 4);
    for (unsigned int dim = 0; dim<dmaif.getNumDims(); dim++)
      opers.erase(opers.begin() + dmaif.getNumDims()+4);
  }
  else {
    opers.erase(opers.begin() + 3);
    for (unsigned int dim = 0; dim<dmaif.getNumDims(); dim++)
      opers.erase(opers.begin() + 4);
  }

  SmallVector<Type, 16> tys;
  for (int i=0,e=opers.size(); i<e; i++) {
    if (opers[i].getType().isa<IndexType>()) {
      opers[i] = rewriter.create<IndexCastOp>(
        op->getLoc(), opers[i], IntegerType::get(op->getContext(), 64));
    }
  }

  Operation* airrtOp = nullptr;
  if (dmaif.getNumDims() == 1)
    airrtOp = rewriter.create<xilinx::airrt::DmaMemcpyOp>(loc, tys, opers);
  else if (dmaif.getNumDims() == 2)
    airrtOp = rewriter.create<xilinx::airrt::DmaMemcpy2dOp>(loc, tys, opers);
  else if (dmaif.getNumDims() == 4)
    airrtOp = rewriter.create<xilinx::airrt::DmaMemcpy4dOp>(loc, tys, opers);
  rewriter.eraseOp(op);
  return airrtOp;
}

class AIRDmaMemcpyToAIRRtConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpyToAIRRtConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpyOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToAirRt(op, operands, rewriter);
    if (call)
      return success();
    else
      return failure();
  }
};

class AIRDmaMemcpy2dToAIRRtConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpy2dToAIRRtConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpy2dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToAirRt(op, operands, rewriter);
    if (call)
      return success();
    else
      return failure();
  }
};

class AIRDmaMemcpy4dToAIRRtConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpy4dToAIRRtConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpy4dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToAirRt(op, operands, rewriter);
    if (call)
      return success();
    else
      return failure();
  }
};

class AIRDmaMemcpyToMemcpyConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpyToMemcpyConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpyOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToMemcpyFn(op, operands, rewriter, "air_memcpy");
    if (call)
      return success();
    else
      return failure();
  }
};

class AIRDmaMemcpy2dToMemcpyConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpy2dToMemcpyConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpy2dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToMemcpyFn(op, operands, rewriter, "air_memcpy2d");
    if (call)
      return success();
    else
      return failure();
  }
};

class AIRDmaMemcpy4dToMemcpyConversion : public ConversionPattern {
public:
  explicit AIRDmaMemcpy4dToMemcpyConversion(MLIRContext *context)
      : ConversionPattern(xilinx::air::DmaMemcpy4dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto call = convertDmaMemcpyToMemcpyFn(op, operands, rewriter, "air_memcpy4d");
    if (call)
      return success();
    else
      return failure();
  }
};

class L2AllocToAIRRtConversion : public ConversionPattern {
public:
  explicit L2AllocToAIRRtConversion(MLIRContext *context)
      : ConversionPattern(memref::AllocOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto alloc = cast<memref::AllocOp>(op);
    auto type = alloc.getType();
    if (type.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L2) {
      rewriter.replaceOpWithNewOp<xilinx::airrt::AllocOp>(op, type);
      return success();
    }
    return failure();
  }
};

class L2DeallocToAIRRtConversion : public ConversionPattern {
public:
  explicit L2DeallocToAIRRtConversion(MLIRContext *context)
      : ConversionPattern(memref::DeallocOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto dealloc = cast<memref::DeallocOp>(op);
    auto type = dealloc.memref().getType().cast<MemRefType>();
    if (type.getMemorySpaceAsInt() == (int)xilinx::air::MemorySpace::L2) {
      rewriter.replaceOpWithNewOp<xilinx::airrt::DeallocOp>(op, SmallVector<Type>{}, op->getOperands());
      return success();
    }
    return failure();
  }
};

class AIRLoweringPass : public PassWrapper<AIRLoweringPass,
                                           OperationPass<ModuleOp>> {

  MemRefType convertTensorType(TensorType tensor) {
    return mlir::MemRefType::get(tensor.getShape(), tensor.getElementType(), {}, 0);
  }

public:

  AIRLoweringPass() = default;
  AIRLoweringPass(const AIRLoweringPass &pass) {}

  Option<bool>
  lowerToCpu{*this, "lower-to-cpu",
              llvm::cl::desc("Lower for cpu emulation"),
              llvm::cl::init(false)};

  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
    registry.insert<AffineDialect,
                    xilinx::airrt::AIRRtDialect,
                    LLVM::LLVMDialect>();
  }

  void runOnOperation() override {

    auto module = getOperation();
    auto context = module.getContext();

    TypeConverter typeConverter;

    OwningRewritePatternList air_patterns(&getContext());

    if (lowerToCpu) {
      // lower to cpu memcpy
      air_patterns.insert<AIRDmaMemcpyToMemcpyConversion,
                          AIRDmaMemcpy2dToMemcpyConversion,
                          AIRDmaMemcpy4dToMemcpyConversion>(context);
    }
    else {
      // lower to air runtime
      air_patterns.insert<AIRDmaMemcpyToAIRRtConversion,
                          AIRDmaMemcpy2dToAIRRtConversion,
                          AIRDmaMemcpy4dToAIRRtConversion,
                          L2AllocToAIRRtConversion,
                          L2DeallocToAIRRtConversion>(context);
    }
    air_patterns.insert<AIRHerdLaunchConversion>(context);
    // air_patterns.insert<AIRHerdLaunchConversion,
    //                     NoOpConversion_affine,
    //                     AIRConv2dConversion,
    //                     AIRConv2dReLUConversion,
    //                     AIRConv2dBatchNormReLUConversion>(context);

    mlir::populateFuncOpTypeConversionPattern(air_patterns,
                                              typeConverter);

    ConversionTarget target(*context);

    target.addLegalDialect<LLVM::LLVMDialect,
                          StandardOpsDialect,
                          AffineDialect,
                          scf::SCFDialect,
                          xilinx::airrt::AIRRtDialect>();

    target.addDynamicallyLegalOp<memref::AllocOp>([&](memref::AllocOp op) {
      return (op.getType().getMemorySpaceAsInt() != (int)xilinx::air::MemorySpace::L2);
    });

    target.addDynamicallyLegalOp<memref::DeallocOp>([&](memref::DeallocOp op) {
      return (op.memref().getType().cast<MemRefType>().getMemorySpaceAsInt() != 
              (int)xilinx::air::MemorySpace::L2);
    });

    if (failed(applyPartialConversion(module, target, std::move(air_patterns)))) {
      emitError(UnknownLoc::get(context), "error lowering ATen\n");
      signalPassFailure();
    }
  }
};

} // namespace

namespace xilinx {
namespace air {

std::unique_ptr<mlir::Pass> createAIRLoweringPass() {
  return std::make_unique<AIRLoweringPass>();
}

} // namespace air
} // namespace xilinx
