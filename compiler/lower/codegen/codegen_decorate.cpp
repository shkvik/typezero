// Decorators (docs/decorators.md): what a class declaration runs for them,
// and the methods a decorator may replace.
//
// Sema made the decorators into statements (ClassDecl::decorations), which
// run where the class is declared, after its static fields get their values.
//
// A method whose decorator takes its descriptor may have its code replaced:
// `descriptor.value = function (this: unknown, ...args: A): R { ... }`. Its
// calls go through the hidden global FunctionDecl::impl, a function of the
// descriptor's `value` type, `(this: unknown, ...the method's parameters) =>
// R`: the method's code is a stub that calls what the global holds, with the
// object (or the class) as its `this`. Before the decorators run, the global
// holds the adapter, which checks the `this` and runs the method's own body,
// emitted as `.original`. So every call, through the class, a method table,
// an interface or a bound value, sees what the decorators left.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

void CodeGen::emitClassDeclaration(ClassDecl *cls) {
  // A base that is a value (`extends Mixin(Base)`) is evaluated first.
  if (cls->baseExpr)
    emitDiscarded(cls->baseExpr);
  emitStaticFields(cls);
  for (Stmt *stmt : cls->decorations)
    emitStmt(stmt);
}

// The class of a class expression is one class, made once, however many
// times the expression runs (docs/decorators.md, section 5): its base is
// evaluated each time, and its static fields get their values the first.
llvm::Value *CodeGen::emitClassExpr(ClassExpr *expr) {
  ClassDecl *cls = expr->decl;
  if (cls->baseExpr)
    emitDiscarded(cls->baseExpr);
  auto initialized = [](const ClassField &f) { return f.isStatic && f.var && f.init; };
  if (!cls->decorations.empty() || llvm::any_of(cls->fields, initialized)) {
    auto *done = new llvm::GlobalVariable(*module_, builder_.getInt1Ty(), /*isConstant=*/false,
                                          llvm::GlobalValue::InternalLinkage, builder_.getFalse(),
                                          "tz.classinit");
    llvm::BasicBlock *initBlock = newBlock("class.init");
    llvm::BasicBlock *doneBlock = newBlock("class.ready");
    builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), done), doneBlock, initBlock);
    switchTo(initBlock);
    builder_.CreateStore(builder_.getTrue(), done);
    emitStaticFields(cls);
    for (Stmt *stmt : cls->decorations)
      emitStmt(stmt);
    builder_.CreateBr(doneBlock);
    switchTo(doneBlock);
  }
  return classObject(cls);
}

llvm::Constant *CodeGen::implAdapterInfo(FunctionDecl *fn) {
  if (llvm::Constant *found = adapterInfos_.lookup(fn))
    return found;
  const Type *implType = fn->impl->type;
  llvm::Function *original = originalCode(fn);
  std::string name = methodCode(fn)->getName().str();
  auto *adapter = llvm::Function::Create(closureFnType(implType), llvm::GlobalValue::InternalLinkage,
                                         name + ".adapter", module_.get());
  setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      fnInfoTy_, {adapter, stringLiteral(std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global = new llvm::GlobalVariable(*module_, fnInfoTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, info, name + ".info");
  adapterInfos_[fn] = global;

  // (env, this: unknown, the parameters) -> R: the `this` must be an instance
  // of the class (a static method takes any), then the body gets the rest.
  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", adapter));
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = implType->ret();
  auto finish = [&](llvm::Value *result) {
    builder_.CreateCall(instanceReleaseFn_, {self});
    if (ret->isVoid())
      builder_.CreateRetVoid();
    else
      builder_.CreateRet(result);
  };
  const ClassDecl *cls = fn->cls;
  if (!fn->isStatic) {
    auto *failBlock = llvm::BasicBlock::Create(ctx_, "this.wrong", adapter);
    auto *testBlock = llvm::BasicBlock::Create(ctx_, "this.test", adapter);
    auto *callBlock = llvm::BasicBlock::Create(ctx_, "this.ok", adapter);
    builder_.CreateCondBr(builder_.CreateIsNotNull(self), testBlock, failBlock);
    builder_.SetInsertPoint(testBlock);
    // The number of its class, which a box and a class do not have.
    llvm::Value *layout = builder_.CreateLoad(
        ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), self, 8), "layout");
    llvm::Value *id = builder_.CreateLoad(
        builder_.getInt32Ty(), builder_.CreateStructGEP(layoutDescTy_, layout, 6), "class");
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *matches = builder_.CreateICmpULE(builder_.CreateSub(id, builder_.getInt32(cls->id)),
                                                  builder_.getInt32(last - cls->id));
    builder_.CreateCondBr(matches, callBlock, failBlock);
    builder_.SetInsertPoint(failBlock);
    std::string message = cls->name.str() + "." + fn->name.str() +
                          " was called with a 'this' that is not an instance of " + cls->name.str();
    llvm::Value *text = stringLiteral(std::u16string(message.begin(), message.end()));
    builder_.CreateCall(unknownFunction("tz_throw_str", builder_.getVoidTy(),
                                        {builder_.getInt32Ty(), ptr, builder_.getInt64Ty()}),
                        {builder_.getInt32(1), stringObj(text), stringMeta(text)});
    // The arguments go with the call that does not happen.
    for (size_t i = 1; i < implType->params().size(); ++i)
      releaseValue(adapter->getArg(unsigned(i + 1)), implType->params()[i]);
    finish(ret->isVoid() ? nullptr : llvm::Constant::getNullValue(lower(ret)));
    builder_.SetInsertPoint(callBlock);
  }
  std::vector<llvm::Value *> args;
  if (!fn->isStatic)
    args.push_back(self); // borrowed, as a method takes its object
  for (unsigned i = 2; i < adapter->arg_size(); ++i)
    args.push_back(adapter->getArg(i));
  llvm::CallInst *result = builder_.CreateCall(original, args);
  finish(ret->isVoid() ? nullptr : result);
  return global;
}

llvm::Function *CodeGen::originalCode(FunctionDecl *fn) {
  llvm::Function *&original = originals_[fn];
  if (!original) {
    llvm::Function *stub = methodCode(fn);
    original = llvm::Function::Create(stub->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                      stub->getName() + ".original", module_.get());
    setFunctionAttributes(original);
  }
  return original;
}

void CodeGen::emitDecoratedMethod(FunctionDecl *fn) {
  // The body, as any method's or static method's.
  llvm::Function *original = originalCode(fn);
  if (fn->isGenerator) {
    emitGenerator(fn, original);
  } else if (fn->isAsync) { // a coroutine, whose promise the stub gives
    emitAsyncFunction(fn, original);
  } else {
    beginFunction(original, fn);
    std::vector<llvm::Value *> args;
    unsigned first = 0;
    if (fn->takesThis()) {
      envParam_ = original->getArg(0);
      envParam_->setName("this");
      first = 1;
    }
    for (unsigned i = first; i < original->arg_size(); ++i)
      args.push_back(original->getArg(i));
    emitParamsAndBody(fn, args);
    finishFunction();
  }

  // The stub: a call of what the global holds, with the object (or the
  // class) as its `this`, and the arguments as they are.
  llvm::Function *stub = methodCode(fn);
  const Type *implType = fn->impl->type;
  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", stub));
  unsigned first = fn->takesThis() ? 1 : 0;
  llvm::Value *self = fn->takesThis() ? static_cast<llvm::Value *>(stub->getArg(0))
                                      : classObject(fn->cls);
  retainObject(self); // the callee owns its `this`
  std::vector<llvm::Value *> args{self};
  for (unsigned i = first; i < stub->arg_size(); ++i)
    args.push_back(stub->getArg(i));
  // Kept alive during the call, which may replace it.
  llvm::Value *impl = builder_.CreateLoad(fnTy_, addressOf(fn->impl), "impl");
  llvm::Value *env = builder_.CreateExtractValue(impl, 1);
  builder_.CreateCall(objRetainFn_, {env});
  llvm::Value *result = callFunctionValue(impl, implType, args);
  builder_.CreateCall(objReleaseFn_, {env});
  if (result)
    builder_.CreateRet(result);
  else
    builder_.CreateRetVoid();
}

// Metadata (runtime/meta.c): the key as an address of a constant of its own
// for an object key, or a string; values go in as unknowns, and records of an
// object key come out as the type Sema found for them.
llvm::Value *CodeGen::emitMetaCall(CallExpr *call) {
  usesMetadata_ = true;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  bool define = call->builtin == Builtin::MetaDefine;
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  llvm::Value *key = values[0];
  llvm::Value *target = values[define ? 2 : 1];
  llvm::Value *property = values[define ? 3 : 2];
  const Type *propertyType = call->args[define ? 3 : 2]->type;
  llvm::Value *keyId = call->metaKey ? metaKeyConstant(call->metaKey)
                                     : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value *has = optionalHas(property, propertyType);
  llvm::Value *text = optionalValue(property, propertyType);
  std::vector<llvm::Value *> args{keyId, stringObj(key), stringMeta(key)};
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::MetaDefine:
    args.insert(args.end(), {values[1], target, stringObj(text), stringMeta(text),
                             builder_.CreateZExt(has, i32), builder_.getInt32(call->designRecord)});
    builder_.CreateCall(unknownFunction("tz_meta_define", builder_.getVoidTy(),
                                        {ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32}),
                        args);
    break;
  case Builtin::MetaDelete:
    args.insert(args.end(), {target, stringObj(text), stringMeta(text), builder_.CreateZExt(has, i32)});
    result = builder_.CreateICmpNE(
        builder_.CreateCall(unknownFunction("tz_meta_delete", i32, {ptr, ptr, i64, ptr, ptr, i64, i32}),
                            args),
        builder_.getInt32(0));
    break;
  default: {
    bool own = call->builtin == Builtin::MetaGetOwn || call->builtin == Builtin::MetaHasOwn;
    args.insert(args.end(), {target, stringObj(text), stringMeta(text), builder_.CreateZExt(has, i32),
                             builder_.getInt32(own)});
    bool get = call->builtin == Builtin::MetaGet || call->builtin == Builtin::MetaGetOwn;
    llvm::Value *found = builder_.CreateCall(
        unknownFunction(get ? "tz_meta_get" : "tz_meta_has", get ? ptr : i32,
                        {ptr, ptr, i64, ptr, ptr, i64, i32, i32}),
        args, "record");
    if (!get) {
      result = builder_.CreateICmpNE(found, builder_.getInt32(0));
      break;
    }
    if (call->type->isUnknown()) {
      result = found;
      break;
    }
    // A record of the type Sema knows it has, or undefined.
    const Type *type = call->type->nonOptional();
    llvm::BasicBlock *readBlock = newBlock("record.read");
    llvm::BasicBlock *endBlock = newBlock("record.end");
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    builder_.CreateCondBr(builder_.CreateIsNotNull(found), readBlock, endBlock);
    switchTo(readBlock);
    llvm::Value *slot = createStackSlot(lower(type), "record.value");
    builder_.CreateCall(unknownFunction("tz_unknown_unbox", builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {found, typeDescriptor(type), slot});
    llvm::Value *value = makeOptional(builder_.CreateLoad(lower(type), slot), builder_.getTrue(),
                                      call->type);
    builder_.CreateCall(instanceReleaseFn_, {found});
    llvm::BasicBlock *readEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(call->type), 2);
    phi->addIncoming(undefinedValue(call->type), entry);
    phi->addIncoming(value, readEnd);
    result = phi;
    break;
  }
  }
  releaseValue(key, Type::string());
  releaseValue(target, Type::anyObject());
  releaseValue(property, propertyType);
  if (define && !call->metaKey && !call->designRecord && !isa<StringLitExpr>(call->args[0]))
    emitThrowCheck(); // a key `design:*` the program wrote
  return result;
}

llvm::Constant *CodeGen::metaKeyConstant(const void *key) {
  llvm::Constant *&global = metaKeys_[key];
  if (!global)
    global = new llvm::GlobalVariable(*module_, builder_.getInt8Ty(), /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage, builder_.getInt8(0),
                                      "tz.meta.key");
  return global;
}

// With metadata, the runtime finds the class of an instance target by its
// number, and goes up from there: every class gets its object.
void CodeGen::finishMetadata(SourceFile &file, llvm::Instruction *init) {
  if (!usesMetadata_)
    return;
  std::vector<const ClassDecl *> classes;
  unsigned count = 1;
  for (Stmt *item : file.items)
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && !cls->literal) {
      classes.push_back(cls);
      count = std::max(count, cls->id + 1);
    }
  std::vector<llvm::Constant *> table(count, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
  for (const ClassDecl *cls : classes)
    if (cls->id)
      table[cls->id] = classObject(cls);
  auto *type = llvm::ArrayType::get(builder_.getPtrTy(), count);
  auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(type, table), "tz.classes");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(unknownFunction("tz_meta_init", builder_.getVoidTy(),
                               {builder_.getPtrTy(), builder_.getInt32Ty()}),
               {global, b.getInt32(count)});
}

} // namespace tz
