#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// An object type that only the drop function of its layout tells apart: an
// object literal type, an interface, a tuple.
bool isPlainObject(const Type *type) {
  return type->isObject() && !type->isClass() && !type->isAnyObject() && !type->isBytes() &&
         !type->isDate() && !type->isRegExp() && !type->isMatch() && !type->isIntl() &&
         !type->isCrypto() && !type->isRuntimeObject() && !type->isTimer() && type->bases().empty();
}

} // namespace

StreamLowerer::StreamLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *StreamLowerer::emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = codegen_.functionFrame().newBlock("chunk.fits");
    codegen_.builder_.CreateCondBr(ok, next, fail);
    codegen_.functionFrame().switchTo(next);
  };
  // Binary data is the object itself, of the kind its layout says.
  if (to->isBytes()) {
    llvm::Value *ok = codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_lib_is_bytes", i32, {ptr, i32}),
                                          {u, codegen_.builder_.getInt32(uint32_t(to->bytes()))});
    test(codegen_.builder_.CreateICmpNE(ok, codegen_.builder_.getInt32(0)));
    codegen_.refCounts().retainValue(u, to);
    return u;
  }
  // Objects of one object type share the drop function of their layouts,
  // whatever the order their fields were given in.
  if (isPlainObject(to)) {
    llvm::Value *layout =
        codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
    test(codegen_.builder_.CreateIsNotNull(layout));
    llvm::Value *drop = codegen_.builder_.CreateLoad(ptr, layout, "drop");
    test(codegen_.builder_.CreateICmpEQ(drop, codegen_.objectLayouts().objectLayout(to).drop));
    codegen_.refCounts().retainValue(u, to);
    return u;
  }

  return codegen_.unknownCastLowerer().emitUnknownAs(u, to, fail);
}

llvm::Value *StreamLowerer::emitStreamBuiltin(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::LibChunk: {
    // The library's types promise what the unknown holds; anything else is
    // a fault of tz, which ends the program with what it is.
    const Type *to = call->type;
    CodeGen::Operand u = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    if (to->isUnknown()) {
      codegen_.refCounts().retainValue(u.value, to);
      llvm::Value *value = u.value;
      codegen_.refCounts().dropOperand(u);
      return value;
    }
    llvm::BasicBlock *fail = codegen_.functionFrame().newBlock("chunk.bad");
    llvm::Value *value = emitChunkAs(u.value, to, fail);
    llvm::BasicBlock *here = codegen_.builder_.GetInsertBlock();
    codegen_.functionFrame().switchTo(fail);
    llvm::StringRef name = to->name();
    codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_lib_bad_chunk", codegen_.builder_.getVoidTy(), {ptr, ptr, i32}),
                        {u.value, codegen_.stringLowerer().utf8Constant(name), codegen_.builder_.getInt32(uint32_t(name.size()))});
    codegen_.builder_.CreateUnreachable();
    codegen_.builder_.SetInsertPoint(here); // a block of the function already
    codegen_.refCounts().dropOperand(u);
    return value;
  }
  case Builtin::LibIsChunk: {
    // The type it tests for is that of the second argument, which Sema
    // adds and which is not evaluated.
    const Type *wanted = call->args[1]->type;
    CodeGen::Operand u = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::BasicBlock *fail = codegen_.functionFrame().newBlock("chunk.not");
    llvm::BasicBlock *end = codegen_.functionFrame().newBlock("chunk.end");
    llvm::Value *value = emitChunkAs(u.value, wanted, fail);
    codegen_.refCounts().releaseValue(value, wanted);
    llvm::BasicBlock *fits = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(end);
    codegen_.functionFrame().switchTo(fail);
    codegen_.builder_.CreateBr(end);
    codegen_.functionFrame().switchTo(end);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2, "is.chunk");
    phi->addIncoming(codegen_.builder_.getTrue(), fits);
    phi->addIncoming(codegen_.builder_.getFalse(), fail);
    codegen_.refCounts().dropOperand(u);
    return phi;
  }
  case Builtin::LibOverrides: {
    // The code at the method's slot in the table of the object's class is
    // not the method the code knows.
    const FunctionDecl *method = call->method ? call->method->decl : nullptr;
    CodeGen::Operand object = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *result = codegen_.builder_.getFalse();
    if (method && method->slot >= 0 && method->body) {
      result = codegen_.builder_.CreateICmpNE(codegen_.inheritLowerer().vtableEntry(object.value, method->slot, /*info=*/false),
                                     codegen_.classDescriptors().methodCode(method), "overrides");
    }
    codegen_.refCounts().dropOperand(object);
    return result;
  }
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk: {
    // A Promise that the runtime settles (runtime/library/node/fs/fs.c), as
    // those of node:fs/promises are (codegen_async_io.cpp).
    llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i64 = codegen_.builder_.getInt64Ty();
    llvm::Type *f64 = codegen_.builder_.getDoubleTy();
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *promise = codegen_.promiseLowerer().newPromise(call->type->promised());
    auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                   std::vector<llvm::Value *> args, bool maker) {
      params.insert(params.begin(), ptr);
      args.insert(args.begin(), promise);
      if (maker) {
        params.push_back(ptr);
        args.push_back(codegen_.builtinModuleLowerer().systemErrorMaker());
      }
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction(name, voidTy, params), args);
    };
    switch (call->builtin) {
    case Builtin::LibFsOpen:
      run("tz_fss_open", {ptr, i64, ptr, i64, i32},
          {codegen_.stringLowerer().stringObj(values[0]), codegen_.stringLowerer().stringMeta(values[0]), codegen_.stringLowerer().stringObj(values[1]), codegen_.stringLowerer().stringMeta(values[1]),
           values[2]},
          true);
      break;
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
      run(call->builtin == Builtin::LibFsRead ? "tz_fss_read" : "tz_fss_write",
          {i32, ptr, i32, i32, f64}, {values[0], values[1], values[2], values[3], values[4]}, true);
      break;
    case Builtin::LibFsClose:
      run("tz_fss_close", {i32, i32}, {values[0], codegen_.builder_.CreateZExt(values[1], i32)}, true);
      break;
    default:
      run("tz_stdin_chunk", {i32}, {values[0]}, false);
      break;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      codegen_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
    return promise;
  }
  case Builtin::LibStdWrite: {
    // The bytes of the Buffer, where console.log writes
    // (runtime/core/tzrt/tzrt.c).
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_lib_std_write", codegen_.builder_.getVoidTy(), {i32, ptr}),
                        {values[0], values[1]});
    codegen_.refCounts().releaseValue(values[1], call->args[1]->type);
    return nullptr;
  }
  default:
    return nullptr;
  }
}

} // namespace tz
