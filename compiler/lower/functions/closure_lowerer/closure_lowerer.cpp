#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

// The name of a closure's code in the IR: the functions it is nested in, then
// its own.
std::string qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent) {
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  }

  return "tz." + name;
}

// The function a name refers to directly (see Checker::namedFunction).
FunctionDecl *knownFunction(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name) {
    return nullptr;
  }
  if (name->func) {
    return name->func->decl;
  }
  if (VarSymbol *var = name->var) {
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  }

  return nullptr;
}

} // namespace

ClosureLowerer::ClosureLowerer(Lowering &lowering) : lowering_(lowering) {}

const Type *ClosureLowerer::functionTypeOf(const FuncSymbol *fn) {
  return Type::function(fn->params, fn->minArgs, fn->hasRest, fn->ret, fn->hasThis);
}

const Type *ClosureLowerer::typeOf(const VarSymbol *var) {
  if (var->type) {
    return var->type;
  }
  const FunctionDecl *fn = var->function ? var->function : var->lambda;

  return functionTypeOf(fn->sym);
}

llvm::FunctionType *ClosureLowerer::closureFnType(const Type *fnType) {
  std::vector<llvm::Type *> params{lowering_.builder_.getPtrTy()};
  for (const Type *param : fnType->params()) {
    params.push_back(lowering_.moduleBuilder().lower(param));
  }

  return llvm::FunctionType::get(lowering_.moduleBuilder().lower(fnType->ret()), params, false);
}

llvm::Value *ClosureLowerer::makeFunctionValue(llvm::Value *info, llvm::Value *env) {
  if (auto *infoConst = dyn_cast<llvm::Constant>(info)) {
    if (auto *envConst = dyn_cast<llvm::Constant>(env)) {
      return llvm::ConstantStruct::get(lowering_.fnTy_, {infoConst, envConst});
    }
  }
  llvm::Value *value = llvm::UndefValue::get(lowering_.fnTy_);
  value = lowering_.builder_.CreateInsertValue(value, info, 0);

  return lowering_.builder_.CreateInsertValue(value, env, 1);
}

llvm::Function *ClosureLowerer::closureCode(FunctionDecl *fn) {
  if (llvm::Function *code = closureCode_.lookup(fn)) {
    return code;
  }
  auto *code = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                                      llvm::GlobalValue::InternalLinkage, qualifiedName(fn),
                                      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(code);
  closureCode_[fn] = code;
  lowering_.pendingClosures_.push_back(fn);

  return code;
}

llvm::Constant *ClosureLowerer::functionInfo(FunctionDecl *fn) {
  if (llvm::Constant *info = infos_.lookup(fn)) {
    return info;
  }
  llvm::Function *code;
  if (fn->takesThis()) {
    code = lowering_.classDescriptors().methodCode(fn); // the object is its environment
  } else if (fn->isClosure()) {
    code = closureCode(fn);
  } else {
    // A top-level function as a value: code that takes an environment and
    // calls it.
    llvm::Function *target = lowering_.functions_.lookup(fn->sym);
    code = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                                  llvm::GlobalValue::InternalLinkage, "tz." + fn->name + ".value",
                                  lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i) {
      args.push_back(code->getArg(i));
    }
    llvm::CallInst *result = b.CreateCall(target, args);
    if (result->getType()->isVoidTy()) {
      b.CreateRetVoid();
    } else {
      b.CreateRet(result);
    }
  }
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(utf16(fn->name))}),
      "tz.fninfo." + fn->name);
  infos_[fn] = global;

  return global;
}

llvm::Value *ClosureLowerer::emitFunctionExpr(FunctionExpr *fe) {
  FunctionDecl *fn = fe->fn;
  llvm::Constant *info = functionInfo(fn);
  const Lowering::EnvLayout &layout = lowering_.envLowerer().envLayout(fn);
  // Without captures there is no environment, unless the program compares
  // functions: then each evaluation needs an identity of its own.
  llvm::Value *env = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (!layout.fields.empty() || (lowering_.comparesFunctions_ && !fn->nonEscaping)) {
    env = lowering_.envLowerer().createEnv(layout, fn->nonEscaping,
                                           !lowering_.memory().unstoredSites.contains(fe));
    lowering_.groupLowerer().noteUnstored(env, fe);
  }

  return makeFunctionValue(info, env);
}

llvm::Value *ClosureLowerer::closureEnv(CallExpr *call) {
  FunctionDecl *fn = call->func->decl;
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (lowering_.currentFn_ &&
      (lowering_.currentFn_ == fn || (fn->group && lowering_.currentFn_->group == fn->group))) {
    return lowering_.envParam_ ? lowering_.envParam_ : null;
  }
  if (fn->group && fn->group->owner == lowering_.currentFn_) {
    return lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(),
                                         lowering_.groupEnvs_.lookup(fn->group));
  }
  // Through the variable that holds it, which cannot change.
  const VarSymbol *var = cast<NameExpr>(call->callee)->var;

  return lowering_.builder_.CreateExtractValue(
      lowering_.envLowerer().emitVarValue(var, /*borrow=*/true), 1);
}

llvm::Value *ClosureLowerer::callFunctionValue(llvm::Value *fn, const Type *fnType,
                                               llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Value *> all{lowering_.builder_.CreateExtractValue(fn, 1)};
  all.insert(all.end(), args.begin(), args.end());
  for (size_t i = args.size(); i < fnType->params().size(); ++i) {
    all.push_back(lowering_.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *info = lowering_.builder_.CreateExtractValue(fn, 0);
  llvm::Value *code = lowering_.builder_.CreateLoad(
      lowering_.builder_.getPtrTy(),
      lowering_.builder_.CreateStructGEP(lowering_.fnInfoTy_, info, 0), "code");
  llvm::CallInst *result = lowering_.builder_.CreateCall(closureFnType(fnType), code, all);

  return result->getType()->isVoidTy() ? nullptr : result;
}

// `f(x)` with a function value f: the function stays alive during the call.
// One in a field, `o.f(x)`, is not borrowed: the call may assign to the field.
llvm::Value *ClosureLowerer::emitValueCall(CallExpr *call) {
  // A method of an interface: the code the object's table gives.
  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    if (const Lowering::InterfaceMember *method = lowering_.inheritLowerer().dispatchedMember(
            member->object->type, member->name, call->signature);
        (method && method->callable) || call->signature >= 0) {
      return lowering_.inheritLowerer().emitInterfaceCall(call);
    }
  }
  const Type *type = call->callee->type;
  Lowering::Operand callee = lowering_.refCounts().emitOperand(
      call->callee,
      isa<NameExpr>(call->callee) && llvm::all_of(call->args, AssignLowerer::cannotAssign));
  lowering_.throwLowerer().holdOperand(callee, llvm::any_of(call->args, [&](Expr *arg) {
                                         return lowering_.mayThrow().exprThrows(arg);
                                       }));
  std::vector<llvm::Value *> args = lowering_.throwLowerer().emitHeldValues(call->args);
  lowering_.throwLowerer().takeOperand(callee);
  llvm::Value *result = callFunctionValue(callee.value, type, args);
  lowering_.refCounts().dropOperand(callee);
  lowering_.emitThrowCheckIf(lowering_.typeThrows(type));

  return result;
}

// A function known by name, given a type that passes more or fewer
// arguments or drops the result: the same environment, with code that adapts
// the call.
llvm::Value *ClosureLowerer::emitFunctionAdapter(CastExpr *cast) {
  FunctionDecl *fn = knownFunction(cast->operand);
  assert(fn && "Checker adapts only functions known by name");
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = lowering_.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = adapters_[{fn, to}];
  if (!info) {
    auto *code = llvm::Function::Create(closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        qualifiedName(fn) + ".adapter", lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
    lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    llvm::ArrayRef<const Type *> params = from->params(), given = to->params();
    for (size_t i = 0; i < params.size(); ++i) {
      if (i >= given.size()) {
        args.push_back(lowering_.optionalLowerer().undefinedValue(params[i]));
        continue;
      }
      llvm::Value *arg = code->getArg(unsigned(i + 1));
      args.push_back(params[i] == given[i] ? arg
                                           : lowering_.optionalLowerer().makeOptional(
                                                 arg, lowering_.builder_.getTrue(), params[i]));
    }
    // The arguments it does not take are released here: the callee owns them.
    for (size_t i = params.size(); i < given.size(); ++i) {
      lowering_.refCounts().releaseValue(code->getArg(unsigned(i + 1)), given[i]);
    }
    llvm::CallInst *result;
    if (fn->isClosure()) {
      args.insert(args.begin(), code->getArg(0));
      result = lowering_.builder_.CreateCall(closureCode(fn), args);
    } else {
      result = lowering_.builder_.CreateCall(lowering_.functions_.lookup(fn->sym), args);
    }
    if (to->ret()->isVoid()) {
      if (!from->ret()->isVoid()) {
        lowering_.refCounts().releaseValue(result, from->ret());
      }
      lowering_.builder_.CreateRetVoid();
    } else {
      lowering_.builder_.CreateRet(result);
    }
    lowering_.builder_.restoreIP(point);
    info = new llvm::GlobalVariable(
        *lowering_.module_, lowering_.fnInfoTy_, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                  {code, lowering_.stringLowerer().stringLiteral(utf16(fn->name))}),
        "tz.fninfo." + fn->name);
  }

  // The environment's reference moves to the new value.
  return makeFunctionValue(info, lowering_.builder_.CreateExtractValue(value, 1));
}

void ClosureLowerer::emitClosure(FunctionDecl *fn) {
  if (fn->isGenerator) {
    lowering_.generatorLowerer().emitGenerator(fn, closureCode(fn));
    return;
  }
  if (fn->isAsync) {
    lowering_.asyncLowerer().emitAsyncFunction(fn, closureCode(fn));
    return;
  }
  lowering_.tailCallLowerer().findTailGroups(fn);
  if (Lowering::TailGroup *group = lowering_.tailGroupOf_.lookup(fn)) {
    lowering_.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  lowering_.functionBuilder().beginFunction(closureCode(fn), fn);
  lowering_.envParam_ = lowering_.function_->getArg(0);
  lowering_.envLowerer().bindCaptures(fn);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < lowering_.function_->arg_size(); ++i) {
    args.push_back(lowering_.function_->getArg(i));
  }
  lowering_.functionBuilder().emitParamsAndBody(fn, args);
  lowering_.functionBuilder().finishFunction();
}

void ClosureLowerer::emitPendingClosures() {
  while (!lowering_.pendingClosures_.empty()) {
    FunctionDecl *fn = lowering_.pendingClosures_.back();
    lowering_.pendingClosures_.pop_back();
    emitClosure(fn);
  }
}

} // namespace tz
