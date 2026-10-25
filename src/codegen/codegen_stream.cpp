// The intrinsics of node:stream (src/lib/stream.ts), which Sema gives as
// Builtin::LibChunk and the others (see src/sema/sema_stream.cpp): the T an
// unknown holds, whether it holds one, and whether the class of an object
// replaces a method.

#include "codegen/codegen.h"

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

llvm::Value *CodeGen::emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = newBlock("chunk.fits");
    builder_.CreateCondBr(ok, next, fail);
    switchTo(next);
  };
  // Binary data is the object itself, of the kind its layout says.
  if (to->isBytes()) {
    llvm::Value *ok =
        builder_.CreateCall(unknownFunction("tz_lib_is_bytes", i32, {ptr, i32}),
                            {u, builder_.getInt32(uint32_t(to->bytes()))});
    test(builder_.CreateICmpNE(ok, builder_.getInt32(0)));
    retainValue(u, to);
    return u;
  }
  // Objects of one object type share the drop function of their layouts,
  // whatever the order their fields were given in.
  if (isPlainObject(to)) {
    llvm::Value *layout =
        builder_.CreateCall(unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
    test(builder_.CreateIsNotNull(layout));
    llvm::Value *drop = builder_.CreateLoad(ptr, layout, "drop");
    test(builder_.CreateICmpEQ(drop, objectLayout(to).drop));
    retainValue(u, to);
    return u;
  }
  return emitUnknownAs(u, to, fail);
}

llvm::Value *CodeGen::emitStreamBuiltin(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::LibChunk: {
    // The library's types promise what the unknown holds; anything else is
    // a fault of tz, which ends the program with what it is.
    const Type *to = call->type;
    Operand u = emitOperand(call->args[0], /*canBorrow=*/true);
    if (to->isUnknown()) {
      retainValue(u.value, to);
      llvm::Value *value = u.value;
      dropOperand(u);
      return value;
    }
    llvm::BasicBlock *fail = newBlock("chunk.bad");
    llvm::Value *value = emitChunkAs(u.value, to, fail);
    llvm::BasicBlock *here = builder_.GetInsertBlock();
    switchTo(fail);
    llvm::StringRef name = to->name();
    builder_.CreateCall(unknownFunction("tz_lib_bad_chunk", builder_.getVoidTy(), {ptr, ptr, i32}),
                        {u.value, utf8Constant(name), builder_.getInt32(uint32_t(name.size()))});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(here); // a block of the function already
    dropOperand(u);
    return value;
  }
  case Builtin::LibIsChunk: {
    // The type it tests for is that of the second argument, which Sema
    // adds and which is not evaluated.
    const Type *wanted = call->args[1]->type;
    Operand u = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::BasicBlock *fail = newBlock("chunk.not");
    llvm::BasicBlock *end = newBlock("chunk.end");
    llvm::Value *value = emitChunkAs(u.value, wanted, fail);
    releaseValue(value, wanted);
    llvm::BasicBlock *fits = builder_.GetInsertBlock();
    builder_.CreateBr(end);
    switchTo(fail);
    builder_.CreateBr(end);
    switchTo(end);
    llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "is.chunk");
    phi->addIncoming(builder_.getTrue(), fits);
    phi->addIncoming(builder_.getFalse(), fail);
    dropOperand(u);
    return phi;
  }
  case Builtin::LibOverrides: {
    // The code at the method's slot in the table of the object's class is
    // not the method the code knows.
    const FunctionDecl *method = call->method ? call->method->decl : nullptr;
    Operand object = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *result = builder_.getFalse();
    if (method && method->slot >= 0 && method->body)
      result = builder_.CreateICmpNE(vtableEntry(object.value, method->slot, /*info=*/false),
                                     methodCode(method), "overrides");
    dropOperand(object);
    return result;
  }
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk: {
    // A Promise that the runtime settles (runtime/fs.c), as those of
    // node:fs/promises are (codegen_async_io.cpp).
    llvm::Type *voidTy = builder_.getVoidTy(), *i64 = builder_.getInt64Ty();
    llvm::Type *f64 = builder_.getDoubleTy();
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *promise = newPromise(call->type->promised());
    auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                   std::vector<llvm::Value *> args, bool maker) {
      params.insert(params.begin(), ptr);
      args.insert(args.begin(), promise);
      if (maker) {
        params.push_back(ptr);
        args.push_back(systemErrorMaker());
      }
      builder_.CreateCall(processFunction(name, voidTy, params), args);
    };
    switch (call->builtin) {
    case Builtin::LibFsOpen:
      run("tz_fss_open", {ptr, i64, ptr, i64, i32},
          {stringObj(values[0]), stringMeta(values[0]), stringObj(values[1]),
           stringMeta(values[1]), values[2]},
          true);
      break;
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
      run(call->builtin == Builtin::LibFsRead ? "tz_fss_read" : "tz_fss_write",
          {i32, ptr, i32, i32, f64}, {values[0], values[1], values[2], values[3], values[4]}, true);
      break;
    case Builtin::LibFsClose:
      run("tz_fss_close", {i32, i32}, {values[0], builder_.CreateZExt(values[1], i32)}, true);
      break;
    default:
      run("tz_stdin_chunk", {i32}, {values[0]}, false);
      break;
    }
    for (size_t i = 0; i < values.size(); ++i)
      releaseValue(values[i], call->args[i]->type);
    return promise;
  }
  case Builtin::LibStdWrite: {
    // The bytes of the Buffer, where console.log writes (runtime/tzrt.c).
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    builder_.CreateCall(processFunction("tz_lib_std_write", builder_.getVoidTy(), {i32, ptr}),
                        {values[0], values[1]});
    releaseValue(values[1], call->args[1]->type);
    return nullptr;
  }
  default:
    return nullptr;
  }
}

} // namespace tz
