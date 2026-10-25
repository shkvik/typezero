// The built-in modules tz has written in tz (src/lib, sema_library.cpp):
// their intrinsics, calls of the runtime.
//
// `function __tz_net_listen(host: string, port: int): object` is a call of
// tz_net_listen in the runtime, which gets what C sees plainly: an int32_t
// for an int and a boolean, an int64_t for a long, a double, a string as its
// two words (the object and the meta, borrowed), and a pointer for a
// reference (borrowed; NULL for undefined). It gives a value the same way,
// a reference owned; a string through a TzStr it writes, passed first; a
// Promise<T> is made here, of the type T, and passed first, for the runtime
// to settle. Last comes the maker of SystemError, for the errors of the
// system it may throw (or reject with). An error it throws is left in
// tz_thrown, as a function of the program leaves it.

#include "codegen/codegen.h"

namespace tz {

void CodeGen::emitExternal(FunctionDecl *fn) {
  llvm::Function *function = functions_.lookup(fn->sym);
  beginFunction(function, fn);
  const FuncSymbol *sym = fn->sym;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  const Type *ret = sym->ret;
  llvm::Value *promise = nullptr, *text = nullptr;
  llvm::Type *cret = builder_.getVoidTy();
  if (ret->isPromise()) {
    promise = newPromise(ret->promised());
    params.push_back(ptr);
    args.push_back(promise);
  } else if (ret->isString()) {
    text = createStackSlot(strTy_, "text.out");
    builder_.CreateStore(llvm::Constant::getNullValue(strTy_), text);
    params.push_back(ptr);
    args.push_back(text);
  } else if (ret->isBool()) {
    cret = i32;
  } else if (!ret->isVoid()) {
    cret = lower(ret);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    llvm::Value *value = function->getArg(unsigned(i));
    const Type *type = sym->params[i];
    if (type->isString()) {
      params.push_back(ptr);
      params.push_back(i64);
      args.push_back(stringObj(value));
      args.push_back(stringMeta(value));
    } else if (type->isBool()) {
      params.push_back(i32);
      args.push_back(builder_.CreateZExt(value, i32));
    } else if (isAbortSignal(type)) {
      // A signal crosses as the runtime's side of it (runtime/signal.c).
      params.push_back(ptr);
      args.push_back(nativeSignal(value, type));
    } else {
      params.push_back(value->getType());
      args.push_back(value);
    }
  }
  params.push_back(ptr);
  args.push_back(systemErrorMaker());
  llvm::StringRef name = fn->name;
  name.consume_front("__");
  llvm::Value *result;
  if (name.starts_with("tz_tls_") && !usesTls_) {
    // A program that tz built without TLS (SourceFile::usesTls) has none to call: the call fails
    // as the runtime words it, rejecting the promise or throwing.
    builder_.CreateCall(processFunction("tz_net_no_tls", builder_.getVoidTy(), {ptr, ptr}),
                        {promise ? promise : llvm::Constant::getNullValue(ptr), args.back()});
    result = cret->isVoidTy() ? nullptr : llvm::Constant::getNullValue(cret);
  } else {
    result = builder_.CreateCall(processFunction(name, cret, params), args);
  }
  // The function owns its arguments; the runtime only borrowed them.
  for (size_t i = 0; i < sym->params.size(); ++i)
    releaseValue(function->getArg(unsigned(i)), sym->params[i]);
  if (promise)
    builder_.CreateRet(promise);
  else if (text)
    builder_.CreateRet(builder_.CreateLoad(strTy_, text));
  else if (ret->isBool())
    builder_.CreateRet(builder_.CreateICmpNE(result, builder_.getInt32(0)));
  else if (ret->isVoid())
    builder_.CreateRetVoid();
  else
    builder_.CreateRet(result);
  finishFunction();
}

llvm::Value *CodeGen::emitResultAdapter(CastExpr *cast) {
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = emitExpr(cast->operand);
  llvm::Constant *&info = resultAdapters_[{from, to}];
  if (!info) {
    auto *code = llvm::Function::Create(closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        "tz.adapt.result", module_.get());
    setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
    llvm::Function *outer = std::exchange(function_, code);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
    // The environment is the box of the function it wraps, which the call
    // borrows; the arguments go to that function, which owns them.
    llvm::Value *inner = builder_.CreateLoad(
        fnTy_, builder_.CreateStructGEP(boxType(from), code->getArg(0), 2), "wrapped");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i)
      args.push_back(code->getArg(i));
    llvm::Value *result = callFunctionValue(inner, from, args);
    // What it gives after it threw is nothing to convert.
    if (typeThrows(from)) {
      llvm::BasicBlock *thrownBlock = newBlock("adapt.thrown");
      llvm::BasicBlock *okBlock = newBlock("adapt.ok");
      llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), thrownSlot(), "thrown");
      builder_.CreateCondBr(builder_.CreateIsNotNull(error), thrownBlock, okBlock);
      switchTo(thrownBlock);
      builder_.CreateRet(llvm::Constant::getNullValue(lower(to->ret())));
      switchTo(okBlock);
    }
    builder_.CreateRet(from->ret() == to->ret() ? result
                                               : emitConversion(result, from->ret(), to->ret()));
    function_ = outer;
    builder_.restoreIP(point);
    info = new llvm::GlobalVariable(*module_, fnInfoTy_, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage,
                                    llvm::ConstantStruct::get(fnInfoTy_,
                                                              {code, stringLiteral(u"")}),
                                    "tz.fninfo.adapt");
  }
  // The box is a member when the function may hold one, as the box of an
  // unknown is (CodeGen::boxValue): a box outside the groups would count
  // from outside it a reference that a group holding the adapter owns.
  bool member = mayHoldMember(from);
  llvm::Value *box = newBox(from, member);
  builder_.CreateStore(value, builder_.CreateStructGEP(boxType(from), box, 2));
  if (member)
    groupStore(box, value, from);
  return makeFunctionValue(info, box);
}

// A key of TZ_FIELD_GETTER for each getter: a TzGetterType with the type of
// the value and a function that calls the getter and writes what it gives.
void CodeGen::appendGetterKeys(llvm::ArrayRef<const ClassDecl *> chain,
                               std::vector<llvm::Constant *> &entries) {
  std::vector<const FunctionDecl *> getters;
  for (const ClassDecl *c : chain)
    for (const FunctionDecl *fn : c->methods) {
      if (fn->accessor != FunctionDecl::Accessor::Getter || fn->isStatic ||
          fn->access != Access::Public || fn->name.starts_with("#") || fn->hiddenGetter)
        continue;
      auto it = llvm::find_if(getters, [&](const FunctionDecl *g) { return g->name == fn->name; });
      if (it == getters.end())
        getters.push_back(fn);
      else
        *it = fn;
    }
  llvm::Type *ptr = builder_.getPtrTy();
  auto *getterTy = llvm::StructType::get(ctx_, {typeDescTy_, ptr, ptr});
  for (const FunctionDecl *fn : getters) {
    llvm::Function *code = fn->sym ? functions_.lookup(fn->sym) : nullptr;
    if (!code)
      continue;
    std::string name = (fn->cls->name + "." + fn->name).str();
    auto *get = llvm::Function::Create(
        llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
        llvm::GlobalValue::InternalLinkage, "tz.get." + name, module_.get());
    setFunctionAttributes(get);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", get));
    llvm::Value *value = b.CreateCall(code, {get->getArg(0)});
    b.CreateStore(value, get->getArg(1));
    b.CreateRetVoid();
    llvm::Constant *base = llvm::ConstantStruct::get(
        typeDescTy_, {builder_.getInt32(17), builder_.getInt32(0),
                      llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr))});
    auto *type = new llvm::GlobalVariable(
        *module_, getterTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(getterTy, {base, typeDescriptor(fn->sym->ret), get}),
        "tz.getter." + name);
    entries.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(fn->name), builder_.getInt32(uint32_t(fn->name.size())),
                       builder_.getInt32(0xFFFFFFFEu), type})); // TZ_FIELD_GETTER
  }
}

} // namespace tz
