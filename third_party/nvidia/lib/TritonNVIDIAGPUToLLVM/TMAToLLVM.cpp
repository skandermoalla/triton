#include "mlir/IR/TypeUtilities.h"

#include "PatternTritonGPUOpToLLVM.h"
#include "TritonNVIDIAGPUToLLVM/PTXAsmFormat.h"

#include "triton/Conversion/TritonGPUToLLVM/Utility.h"
#include "triton/Dialect/Triton/IR/Dialect.h"

using namespace mlir;
using namespace mlir::triton;

namespace {
constexpr int64_t TMA_SIZE_BYTES = 128;

struct TensormapAllocOpConversion
    : public ConvertOpToLLVMPattern<triton::ExperimentalTensormapAllocOp> {
  const NVIDIA::TargetInfo &targetInfo;

  TensormapAllocOpConversion(LLVMTypeConverter &converter,
                             const NVIDIA::TargetInfo &targetInfo,
                             PatternBenefit benefit)
      : ConvertOpToLLVMPattern(converter, benefit), targetInfo(targetInfo) {}

  LogicalResult
  matchAndRewrite(triton::ExperimentalTensormapAllocOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto ctx = getContext();
    Value smemBase =
        LLVM::getSharedMemoryBase(loc, rewriter, op.getOperation());

    auto i32Ty = rewriter.getI32Type();
    constexpr int kWarpSize = 32;
    Value threadId = getThreadId(rewriter, loc);
    Value pred = icmp_slt(threadId, i32_val(kWarpSize));

    // Load template descriptor into registers
    Value tmaVal = [&] {
      auto ptrTy = ptr_ty(ctx, /*addressSpace=*/1);
      auto readAddr = gep(ptrTy, i32Ty, adaptor.getTemplatePtr(), threadId);

      PTXBuilder ptxBuilder;
      auto *dstsOpr = ptxBuilder.newListOperand();
      auto *dstOpr = ptxBuilder.newOperand("=r");
      dstsOpr->listAppend(dstOpr);
      auto *addrOpr = ptxBuilder.newAddrOperand(readAddr, "l");
      auto &ld = ptxBuilder.create<>("ld")->global().b(32);

      ld(dstsOpr, addrOpr).predicate(pred);
      return ptxBuilder.launch(rewriter, loc, i32Ty);
    }();

    // Write to shared memory
    auto ptrTy = ptr_ty(ctx, /*addressSpace=*/3);
    auto writeAddr = gep(ptrTy, i32Ty, smemBase, threadId);
    targetInfo.storeShared(rewriter, loc, writeAddr, tmaVal, pred);

    // Sync warp
    PTXBuilder ptxBuilder;
    auto &bar = *ptxBuilder.create<>("bar.warp.sync");
    auto *maskOpr = ptxBuilder.newConstantOperand(0xffffffff);
    bar(maskOpr);
    ptxBuilder.launch(rewriter, loc, void_ty(ctx));

    rewriter.replaceOp(op, {smemBase});
    return success();
  }
};

struct TensormapDeallocOpConversion
    : public ConvertOpToLLVMPattern<triton::ExperimentalTensormapDeallocOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ExperimentalTensormapDeallocOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // No-op, deallocation is done by allocation analysis
    rewriter.eraseOp(op);
    return success();
  }
};

struct TensormapCpFenceproxyOpConversion
    : public ConvertOpToLLVMPattern<
          triton::ExperimentalTensormapCpFenceproxyOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ExperimentalTensormapCpFenceproxyOp op,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();
    PTXBuilder ptxBuilder;

    // prepare asm operands
    auto *outAddrOpr = ptxBuilder.newAddrOperand(adaptor.getOutPtr(), "l");
    auto *inAddrOpr = ptxBuilder.newAddrOperand(adaptor.getInPtr(), "l");
    auto *sizeOpr = ptxBuilder.newConstantOperand(TMA_SIZE_BYTES);

    // Define the instruction opcode
    auto &cp =
        *ptxBuilder.create<>("tensormap.cp_fenceproxy.global.shared::cta."
                             "tensormap::generic.release.gpu.sync.aligned");

    // Execute collectively on first warp in block
    constexpr int kWarpSize = 32;
    Value threadId = getThreadId(rewriter, loc);
    Value pred = icmp_slt(threadId, i32_val(kWarpSize));

    cp(outAddrOpr, inAddrOpr, sizeOpr).predicate(pred);

    auto ctx = op.getContext();
    ptxBuilder.launch(rewriter, loc, void_ty(ctx));

    rewriter.eraseOp(op);
    return success();
  }
};

struct TensormapFenceproxyAcquireOpConversion
    : public ConvertOpToLLVMPattern<
          triton::ExperimentalTensormapFenceproxyAcquireOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ExperimentalTensormapFenceproxyAcquireOp op,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    auto loc = op.getLoc();
    PTXBuilder ptxBuilder;

    // prepare asm operands
    auto *descAddrOpr = ptxBuilder.newAddrOperand(adaptor.getDescPtr(), "l");
    auto *sizeOpr = ptxBuilder.newConstantOperand(TMA_SIZE_BYTES);

    // Define the instruction opcode
    auto &fence =
        *ptxBuilder.create<>("fence.proxy.tensormap::generic.acquire.gpu");
    fence(descAddrOpr, sizeOpr);

    ptxBuilder.launch(rewriter, loc, getVoidType());

    rewriter.eraseOp(op);
    return success();
  }
};

struct TensormapReplaceOpConversion : public ConvertToLLVMPattern {

  std::string fieldName;

  TensormapReplaceOpConversion(StringRef opName, StringRef fieldName,
                               MLIRContext *context,
                               const LLVMTypeConverter &converter,
                               PatternBenefit benefit)
      : ConvertToLLVMPattern(opName, context, converter, benefit),
        fieldName(fieldName) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {

    auto loc = op->getLoc();
    PTXBuilder ptxBuilder;

    Value descPtr = operands[0];
    Value newVal;
    if (operands.size() == 1) {
    } else {
      newVal = operands[1];
    }

    auto ord = op->getAttrOfType<IntegerAttr>("ord");
    auto newValTy = newVal.getType();
    int width = 0;

    // prepare asm operands
    auto *descAddrOpr = ptxBuilder.newAddrOperand(descPtr, "l");
    PTXInstr::Operand *ordOpr =
        ord ? ptxBuilder.newConstantOperand(ord.getInt()) : nullptr;
    PTXInstr::Operand *newValOpr = nullptr;
    if (operands.size() == 1) {
      auto newVal = op->getAttrOfType<IntegerAttr>("new_val").getInt();
      newValOpr = ptxBuilder.newConstantOperand(newVal);
      width = 32;
    } else {
      assert(operands.size() == 2);
      auto newVal = operands[1];
      auto newValTy = newVal.getType();
      if (mlir::isa<IntegerType>(newValTy)) {
        width = mlir::cast<IntegerType>(newValTy).getWidth();
      } else {
        assert(mlir::isa<mlir::LLVM::LLVMPointerType>(newValTy));
        width = 64;
      }
      const char *constraint = width == 64 ? "l" : "r";
      newValOpr = ptxBuilder.newOperand(operands[1], constraint);
    }

    // Define the instruction opcode
    auto &replace = ptxBuilder.create<>("tensormap.replace.tile")
                        ->o(fieldName)
                        .o("shared::cta")
                        .o("b1024")
                        .o("b32", width == 32)
                        .o("b64", width == 64);

    Value threadId = getThreadId(rewriter, loc);
    Value pred = icmp_eq(threadId, i32_val(0));

    if (ord) {
      replace(descAddrOpr, ordOpr, newValOpr).predicate(pred);
    } else {
      replace(descAddrOpr, newValOpr).predicate(pred);
    }

    ptxBuilder.launch(rewriter, loc, getVoidType());

    rewriter.eraseOp(op);
    return success();
  }
};

} // namespace

void mlir::triton::NVIDIA::populateTMAToLLVMPatterns(
    LLVMTypeConverter &typeConverter, const TargetInfo &targetInfo,
    RewritePatternSet &patterns, PatternBenefit benefit) {
  patterns.add<TensormapAllocOpConversion>(typeConverter, targetInfo, benefit);
  patterns.add<TensormapDeallocOpConversion, TensormapCpFenceproxyOpConversion,
               TensormapFenceproxyAcquireOpConversion>(typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceGlobalAddressOp::getOperationName(),
      "global_address", patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceRankOp::getOperationName(), "rank",
      patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceBoxDimOp::getOperationName(), "box_dim",
      patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceGlobalDimOp::getOperationName(), "global_dim",
      patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceGlobalStrideOp::getOperationName(),
      "global_stride", patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceElementStrideOp::getOperationName(),
      "element_stride", patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceElemTypeOp::getOperationName(), "elemtype",
      patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceInterleaveLayoutOp::getOperationName(),
      "interleave_layout", patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceSwizzleModeOp::getOperationName(),
      "swizzle_mode", patterns.getContext(), typeConverter, benefit);
  patterns.add<TensormapReplaceOpConversion>(
      ExperimentalTensormapReplaceFillModeOp::getOperationName(), "fill_mode",
      patterns.getContext(), typeConverter, benefit);
}
