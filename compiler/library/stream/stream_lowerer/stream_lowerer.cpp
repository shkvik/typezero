#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

bool isPlainObject(const Type *type) {
  return type->isObject() && !type->isClass() && !type->isAnyObject() && !type->isBytes() &&
         !type->isDate() && !type->isRegExp() && !type->isMatch() && !type->isIntl() &&
         !type->isCrypto() && !type->isRuntimeObject() && !type->isTimer() && type->bases().empty();
}

}

StreamLowerer::StreamLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *StreamLowerer::emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = lowering_.functionFrame().newBlock("chunk.fits");
    lowering_.builder_.CreateCondBr(ok, next, fail);
    lowering_.functionFrame().switchTo(next);
  };
  if (to->isBytes()) {
    llvm::Value *ok = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_lib_is_bytes", i32, {ptr, i32}),
        {u, lowering_.builder_.getInt32(uint32_t(to->bytes()))});
    test(lowering_.builder_.CreateICmpNE(ok, lowering_.builder_.getInt32(0)));
    lowering_.refCounts().retainValue(u, to);
    return u;
  }
  if (isPlainObject(to)) {
    llvm::Value *layout = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
    test(lowering_.builder_.CreateIsNotNull(layout));
    llvm::Value *drop = lowering_.builder_.CreateLoad(ptr, layout, "drop");
    test(lowering_.builder_.CreateICmpEQ(drop, lowering_.objectLayouts().objectLayout(to).drop));
    lowering_.refCounts().retainValue(u, to);
    return u;
  }

  return lowering_.unknownCastLowerer().emitUnknownAs(u, to, fail);
}

llvm::Value *StreamLowerer::emitStreamBuiltin(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::LibChunk: {
    const Type *to = call->type;
    Lowering::Operand u = lowering_.refCounts().emitOperand(call->args[0], true);
    if (to->isUnknown()) {
      lowering_.refCounts().retainValue(u.value, to);
      llvm::Value *value = u.value;
      lowering_.refCounts().dropOperand(u);
      return value;
    }
    llvm::BasicBlock *fail = lowering_.functionFrame().newBlock("chunk.bad");
    llvm::Value *value = emitChunkAs(u.value, to, fail);
    llvm::BasicBlock *here = lowering_.builder_.GetInsertBlock();
    lowering_.functionFrame().switchTo(fail);
    llvm::StringRef name = to->name();
    lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_lib_bad_chunk",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr, i32}),
        {u.value, lowering_.stringLowerer().utf8Constant(name),
         lowering_.builder_.getInt32(uint32_t(name.size()))});
    lowering_.builder_.CreateUnreachable();
    lowering_.builder_.SetInsertPoint(here);
    lowering_.refCounts().dropOperand(u);
    return value;
  }
  case Builtin::LibIsChunk: {
    const Type *wanted = call->args[1]->type;
    Lowering::Operand u = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::BasicBlock *fail = lowering_.functionFrame().newBlock("chunk.not");
    llvm::BasicBlock *end = lowering_.functionFrame().newBlock("chunk.end");
    llvm::Value *value = emitChunkAs(u.value, wanted, fail);
    lowering_.refCounts().releaseValue(value, wanted);
    llvm::BasicBlock *fits = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(end);
    lowering_.functionFrame().switchTo(fail);
    lowering_.builder_.CreateBr(end);
    lowering_.functionFrame().switchTo(end);
    llvm::PHINode *phi =
        lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2, "is.chunk");
    phi->addIncoming(lowering_.builder_.getTrue(), fits);
    phi->addIncoming(lowering_.builder_.getFalse(), fail);
    lowering_.refCounts().dropOperand(u);
    return phi;
  }
  case Builtin::LibOverrides: {
    const FunctionDecl *method = call->method ? call->method->decl : nullptr;
    Lowering::Operand object = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *result = lowering_.builder_.getFalse();
    if (method && method->slot >= 0 && method->body) {
      result = lowering_.builder_.CreateICmpNE(
          lowering_.inheritLowerer().vtableEntry(object.value, method->slot, false),
          lowering_.classDescriptors().methodCode(method), "overrides");
    }
    lowering_.refCounts().dropOperand(object);
    return result;
  }
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk: {
    llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i64 = lowering_.builder_.getInt64Ty();
    llvm::Type *f64 = lowering_.builder_.getDoubleTy();
    std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
    llvm::Value *promise = lowering_.promiseLowerer().newPromise(call->type->promised());
    auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                   std::vector<llvm::Value *> args, bool maker) {
      params.insert(params.begin(), ptr);
      args.insert(args.begin(), promise);
      if (maker) {
        params.push_back(ptr);
        args.push_back(lowering_.fsLowerer().systemErrorMaker());
      }
      lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction(name, voidTy, params), args);
    };
    switch (call->builtin) {
    case Builtin::LibFsOpen:
      run("tz_fss_open", {ptr, i64, ptr, i64, i32},
          {lowering_.stringLowerer().stringObj(values[0]),
           lowering_.stringLowerer().stringMeta(values[0]),
           lowering_.stringLowerer().stringObj(values[1]),
           lowering_.stringLowerer().stringMeta(values[1]), values[2]},
          true);
      break;
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
      run(call->builtin == Builtin::LibFsRead ? "tz_fss_read" : "tz_fss_write",
          {i32, ptr, i32, i32, f64}, {values[0], values[1], values[2], values[3], values[4]}, true);
      break;
    case Builtin::LibFsClose:
      run("tz_fss_close", {i32, i32}, {values[0], lowering_.builder_.CreateZExt(values[1], i32)},
          true);
      break;
    default:
      run("tz_stdin_chunk", {i32}, {values[0]}, false);
      break;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
    return promise;
  }
  case Builtin::LibStdWrite: {
    std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_lib_std_write",
                                                   lowering_.builder_.getVoidTy(), {i32, ptr}),
        {values[0], values[1]});
    lowering_.refCounts().releaseValue(values[1], call->args[1]->type);
    return nullptr;
  }
  default:
    return nullptr;
  }
}

}
