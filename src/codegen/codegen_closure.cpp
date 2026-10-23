// Closures: function values, their environments and the boxes of the
// variables they share.
//
// A function value is { ptr info, ptr env }. `info` is a constant
// { ptr code, string name } per function; the code takes the environment as
// its first argument. A top-level function used as a value gets code that
// just calls it, and a null environment.
//
// An environment (and a box) is an object of the runtime (TzObject in
// runtime/tzrt.h): { i64 rc, ptr drop, fields... }. Its fields are the
// captured variables: a copy of the value of one that never changes after the
// closure is created, and the box of any other. The function that declares a
// boxed variable keeps the box in a slot of its frame, and every closure that
// captures it holds a reference to the same box.
//
// The functions declared in one block share one environment (see
// NestedGroup), created before the first statement that mentions them; they
// call each other with it.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// A literal environment on the stack: never freed, so its count only needs
// to stay far from zero.
constexpr uint64_t kStackObjectCount = uint64_t(1) << 62;

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// The name of a closure's code in the IR: the functions it is nested in, then
// its own.
std::string qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent)
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  return "tz." + name;
}

// The function a name refers to directly (see Sema::namedFunction).
FunctionDecl *knownFunction(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name)
    return nullptr;
  if (name->func)
    return name->func->decl;
  if (VarSymbol *var = name->var)
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  return nullptr;
}

} // namespace

const Type *CodeGen::functionTypeOf(const FuncSymbol *fn) {
  return Type::function(fn->params, fn->minArgs, fn->hasRest, fn->ret);
}

const Type *CodeGen::typeOf(const VarSymbol *var) {
  if (var->type)
    return var->type;
  const FunctionDecl *fn = var->function ? var->function : var->lambda;
  return functionTypeOf(fn->sym);
}

llvm::FunctionType *CodeGen::closureFnType(const Type *fnType) {
  std::vector<llvm::Type *> params{builder_.getPtrTy()};
  for (const Type *param : fnType->params())
    params.push_back(lower(param));
  return llvm::FunctionType::get(lower(fnType->ret()), params, false);
}

llvm::Value *CodeGen::makeFunctionValue(llvm::Value *info, llvm::Value *env) {
  if (auto *infoConst = dyn_cast<llvm::Constant>(info))
    if (auto *envConst = dyn_cast<llvm::Constant>(env))
      return llvm::ConstantStruct::get(fnTy_, {infoConst, envConst});
  llvm::Value *value = llvm::UndefValue::get(fnTy_);
  value = builder_.CreateInsertValue(value, info, 0);
  return builder_.CreateInsertValue(value, env, 1);
}

llvm::Function *CodeGen::closureCode(FunctionDecl *fn) {
  if (llvm::Function *code = closureCode_.lookup(fn))
    return code;
  auto *code = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                                      llvm::GlobalValue::InternalLinkage, qualifiedName(fn), module_.get());
  setFunctionAttributes(code);
  closureCode_[fn] = code;
  pendingClosures_.push_back(fn);
  return code;
}

llvm::Constant *CodeGen::functionInfo(FunctionDecl *fn) {
  if (llvm::Constant *info = infos_.lookup(fn))
    return info;
  llvm::Function *code;
  if (fn->isClosure()) {
    code = closureCode(fn);
  } else {
    // A top-level function as a value: code that takes an environment and
    // calls it.
    llvm::Function *target = functions_.lookup(fn->sym);
    code = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)), llvm::GlobalValue::InternalLinkage,
                                  "tz." + fn->name + ".value", module_.get());
    setFunctionAttributes(code);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i)
      args.push_back(code->getArg(i));
    llvm::CallInst *result = b.CreateCall(target, args);
    if (result->getType()->isVoidTy())
      b.CreateRetVoid();
    else
      b.CreateRet(result);
  }
  auto *global = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(utf16(fn->name))}), "tz.fninfo." + fn->name);
  infos_[fn] = global;
  return global;
}

const CodeGen::EnvLayout &CodeGen::envLayout(const FunctionDecl *fn) {
  const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
  auto [it, inserted] = envLayouts_.try_emplace(key);
  EnvLayout &layout = it->second;
  if (!inserted)
    return layout;
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
  layout.fields.assign(captures.begin(), captures.end());
  std::vector<llvm::Type *> types{builder_.getInt64Ty(), builder_.getPtrTy()};
  for (const VarSymbol *var : layout.fields)
    types.push_back(var->boxed() ? builder_.getPtrTy() : lower(typeOf(var)));
  layout.type = llvm::StructType::get(ctx_, types);

  // Releases what the environment holds, then frees it.
  auto *dropType = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  layout.drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                       qualifiedName(fn) + ".env.drop", module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", layout.drop));
  llvm::Value *env = layout.drop->getArg(0);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *field = builder_.CreateStructGEP(layout.type, env, unsigned(i + 2));
    if (var->boxed()) {
      builder_.CreateCall(objReleaseFn_, {builder_.CreateLoad(builder_.getPtrTy(), field)});
    } else {
      const Type *type = typeOf(var);
      releaseValue(builder_.CreateLoad(lower(type), field), type);
    }
  }
  builder_.CreateCall(rt_.objFree, {env});
  builder_.CreateRetVoid();
  builder_.restoreIP(point);
  return layout;
}

llvm::StructType *CodeGen::boxType(const Type *type) {
  llvm::StructType *&box = boxTypes_[type];
  if (!box)
    box = llvm::StructType::get(ctx_, {builder_.getInt64Ty(), builder_.getPtrTy(), lower(type)});
  return box;
}

llvm::Value *CodeGen::newBox(const Type *type) {
  llvm::StructType *box = boxType(type);
  llvm::Function *&drop = boxDrops_[type];
  if (!drop) {
    // Releases the value, then frees the box; without a value to release,
    // just frees it.
    auto *dropType = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
    if (!type->isManaged()) {
      drop = cast<llvm::Function>(rt_.objFree.getCallee());
    } else {
      drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage, "tz.box.drop", module_.get());
      drop->addFnAttr(llvm::Attribute::NoUnwind);
      llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
      builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drop));
      llvm::Value *object = drop->getArg(0);
      releaseValue(builder_.CreateLoad(lower(type), builder_.CreateStructGEP(box, object, 2)), type);
      builder_.CreateCall(rt_.objFree, {object});
      builder_.CreateRetVoid();
      builder_.restoreIP(point);
    }
  }
  uint64_t size = module_->getDataLayout().getTypeAllocSize(box);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "box");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(box, object, 0));
  builder_.CreateStore(drop, builder_.CreateStructGEP(box, object, 1));
  builder_.CreateStore(llvm::Constant::getNullValue(lower(type)), builder_.CreateStructGEP(box, object, 2));
  return object;
}

llvm::Value *CodeGen::boxPointer(const VarSymbol *var) {
  if (llvm::Value *box = envBoxes_.lookup(var))
    return box;
  llvm::Value *slot = boxSlots_.lookup(var);
  assert(slot && "a boxed variable used before its declaration was emitted");
  return builder_.CreateLoad(builder_.getPtrTy(), slot, var->name + ".box");
}

llvm::Value *CodeGen::createEnv(const EnvLayout &layout, bool onStack) {
  llvm::StructType *type = layout.type;
  llvm::Value *env;
  if (onStack) {
    env = createStackSlot(type, "env");
    builder_.CreateStore(builder_.getInt64(kStackObjectCount), builder_.CreateStructGEP(type, env, 0));
    builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.CreateStructGEP(type, env, 1));
  } else {
    uint64_t size = module_->getDataLayout().getTypeAllocSize(type);
    env = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "env");
    builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(type, env, 0));
    builder_.CreateStore(layout.drop, builder_.CreateStructGEP(type, env, 1));
  }
  // An environment on the stack lives only while the variables do, so it
  // borrows their values.
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = boxPointer(var);
      if (!onStack)
        builder_.CreateCall(objRetainFn_, {value});
    } else {
      value = emitVarValue(var, /*borrow=*/onStack);
    }
    builder_.CreateStore(value, builder_.CreateStructGEP(type, env, unsigned(i + 2)));
  }
  return env;
}

llvm::Value *CodeGen::emitVarValue(const VarSymbol *var, bool borrow) {
  // A function's own value: it is the one being emitted (a constant arrow
  // calling itself), or one of the group being emitted or created here.
  const FunctionDecl *fn = nullptr;
  llvm::Value *env = nullptr;
  if (var->lambda && var->lambda == currentFn_) {
    fn = var->lambda;
    env = envParam_;
  } else if (const FunctionDecl *member = var->function) {
    NestedGroup *group = member->group;
    if (currentFn_ && currentFn_->group == group) {
      fn = member;
      env = envParam_;
    } else if (group->owner == currentFn_) {
      fn = member;
      env = builder_.CreateLoad(builder_.getPtrTy(), groupEnvs_.lookup(group));
    }
  }
  if (fn) {
    if (!env)
      env = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    if (!borrow)
      builder_.CreateCall(objRetainFn_, {env});
    return makeFunctionValue(functionInfo(const_cast<FunctionDecl *>(fn)), env);
  }
  const Type *type = typeOf(var);
  llvm::Value *value = builder_.CreateLoad(lower(type), addressOf(var), var->name);
  if (!borrow)
    retainValue(value, type);
  return value;
}

llvm::Value *CodeGen::emitFunctionExpr(FunctionExpr *fe) {
  FunctionDecl *fn = fe->fn;
  llvm::Constant *info = functionInfo(fn);
  const EnvLayout &layout = envLayout(fn);
  // Without captures there is no environment, unless the program compares
  // functions: then each evaluation needs an identity of its own.
  llvm::Value *env = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!layout.fields.empty() || (comparesFunctions_ && !fn->nonEscaping))
    env = createEnv(layout, fn->nonEscaping);
  return makeFunctionValue(info, env);
}

void CodeGen::emitGroupCreation(NestedGroup *group) {
  const EnvLayout &layout = envLayout(group->members.front());
  llvm::Value *slot = createObjectSlot("group.env");
  groupEnvs_[group] = slot;
  if (!layout.fields.empty() || comparesFunctions_)
    builder_.CreateStore(createEnv(layout, /*onStack=*/false), slot);
  for (FunctionDecl *member : group->members)
    closureCode(member);
}

llvm::Value *CodeGen::closureEnv(CallExpr *call) {
  FunctionDecl *fn = call->func->decl;
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (currentFn_ && (currentFn_ == fn || (fn->group && currentFn_->group == fn->group)))
    return envParam_ ? envParam_ : null;
  if (fn->group && fn->group->owner == currentFn_)
    return builder_.CreateLoad(builder_.getPtrTy(), groupEnvs_.lookup(fn->group));
  // Through the variable that holds it, which cannot change.
  const VarSymbol *var = cast<NameExpr>(call->callee)->var;
  return builder_.CreateExtractValue(emitVarValue(var, /*borrow=*/true), 1);
}

llvm::Value *CodeGen::callFunctionValue(llvm::Value *fn, const Type *fnType, llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Value *> all{builder_.CreateExtractValue(fn, 1)};
  all.insert(all.end(), args.begin(), args.end());
  for (size_t i = args.size(); i < fnType->params().size(); ++i)
    all.push_back(undefinedValue(fnType->params()[i]));
  llvm::Value *info = builder_.CreateExtractValue(fn, 0);
  llvm::Value *code = builder_.CreateLoad(builder_.getPtrTy(), builder_.CreateStructGEP(fnInfoTy_, info, 0), "code");
  llvm::CallInst *result = builder_.CreateCall(closureFnType(fnType), code, all);
  return result->getType()->isVoidTy() ? nullptr : result;
}

// `f(x)` with a function value f: the function stays alive during the call.
llvm::Value *CodeGen::emitValueCall(CallExpr *call) {
  const Type *type = call->callee->type;
  Operand callee = emitOperand(call->callee, llvm::all_of(call->args, cannotAssign));
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args)
    args.push_back(emitExpr(arg));
  llvm::Value *result = callFunctionValue(callee.value, type, args);
  dropOperand(callee);
  return result;
}

// A function known by name, given a type that passes more or fewer
// arguments or drops the result: the same environment, with code that adapts
// the call.
llvm::Value *CodeGen::emitFunctionAdapter(CastExpr *cast) {
  FunctionDecl *fn = knownFunction(cast->operand);
  assert(fn && "Sema adapts only functions known by name");
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = emitExpr(cast->operand);
  llvm::Constant *&info = adapters_[{fn, to}];
  if (!info) {
    auto *code = llvm::Function::Create(closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        qualifiedName(fn) + ".adapter", module_.get());
    setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
    builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    llvm::ArrayRef<const Type *> params = from->params(), given = to->params();
    for (size_t i = 0; i < params.size(); ++i) {
      if (i >= given.size()) {
        args.push_back(undefinedValue(params[i]));
        continue;
      }
      llvm::Value *arg = code->getArg(unsigned(i + 1));
      args.push_back(params[i] == given[i] ? arg : makeOptional(arg, builder_.getTrue(), params[i]));
    }
    // The arguments it does not take are released here: the callee owns them.
    for (size_t i = params.size(); i < given.size(); ++i)
      releaseValue(code->getArg(unsigned(i + 1)), given[i]);
    llvm::CallInst *result;
    if (fn->isClosure()) {
      args.insert(args.begin(), code->getArg(0));
      result = builder_.CreateCall(closureCode(fn), args);
    } else {
      result = builder_.CreateCall(functions_.lookup(fn->sym), args);
    }
    if (to->ret()->isVoid()) {
      if (!from->ret()->isVoid())
        releaseValue(result, from->ret());
      builder_.CreateRetVoid();
    } else {
      builder_.CreateRet(result);
    }
    builder_.restoreIP(point);
    info = new llvm::GlobalVariable(*module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                    llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(utf16(fn->name))}),
                                    "tz.fninfo." + fn->name);
  }
  // The environment's reference moves to the new value.
  return makeFunctionValue(info, builder_.CreateExtractValue(value, 1));
}

void CodeGen::bindCaptures(const FunctionDecl *fn) {
  // The captured variables: the values copied into the environment, and the
  // boxes it holds.
  envParam_->setName("env");
  const EnvLayout &layout = envLayout(fn);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *field = builder_.CreateStructGEP(layout.type, envParam_, unsigned(i + 2), var->name);
    if (var->boxed())
      envBoxes_[var] = builder_.CreateLoad(builder_.getPtrTy(), field, var->name + ".box");
    else
      addresses_[var] = field;
  }
}

void CodeGen::emitClosure(FunctionDecl *fn) {
  findTailGroups(fn);
  if (TailGroup *group = tailGroupOf_.lookup(fn)) {
    emitTailGroup(*group);
    return;
  }
  beginFunction(closureCode(fn), fn);
  envParam_ = function_->getArg(0);
  bindCaptures(fn);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < function_->arg_size(); ++i)
    args.push_back(function_->getArg(i));
  emitParamsAndBody(fn, args);
  finishFunction();
}

void CodeGen::emitPendingClosures() {
  while (!pendingClosures_.empty()) {
    FunctionDecl *fn = pendingClosures_.back();
    pendingClosures_.pop_back();
    emitClosure(fn);
  }
}

} // namespace tz
