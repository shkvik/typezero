#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
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

// The function a name refers to directly (see Sema::namedFunction).
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

ClosureLowerer::ClosureLowerer(CodeGen &codegen) : codegen_(codegen) {}

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
  std::vector<llvm::Type *> params{codegen_.builder_.getPtrTy()};
  for (const Type *param : fnType->params()) {
    params.push_back(codegen_.moduleBuilder().lower(param));
  }

  return llvm::FunctionType::get(codegen_.moduleBuilder().lower(fnType->ret()), params, false);
}

llvm::Value *ClosureLowerer::makeFunctionValue(llvm::Value *info, llvm::Value *env) {
  if (auto *infoConst = dyn_cast<llvm::Constant>(info)) {
    if (auto *envConst = dyn_cast<llvm::Constant>(env)) {
      return llvm::ConstantStruct::get(codegen_.fnTy_, {infoConst, envConst});
    }
  }
  llvm::Value *value = llvm::UndefValue::get(codegen_.fnTy_);
  value = codegen_.builder_.CreateInsertValue(value, info, 0);

  return codegen_.builder_.CreateInsertValue(value, env, 1);
}

llvm::Function *ClosureLowerer::closureCode(FunctionDecl *fn) {
  if (llvm::Function *code = closureCode_.lookup(fn)) {
    return code;
  }
  auto *code =
      llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                             llvm::GlobalValue::InternalLinkage, qualifiedName(fn), codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  closureCode_[fn] = code;
  codegen_.pendingClosures_.push_back(fn);

  return code;
}

llvm::Constant *ClosureLowerer::functionInfo(FunctionDecl *fn) {
  if (llvm::Constant *info = infos_.lookup(fn)) {
    return info;
  }
  llvm::Function *code;
  if (fn->takesThis()) {
    code = codegen_.classDescriptors().methodCode(fn); // the object is its environment
  } else if (fn->isClosure()) {
    code = closureCode(fn);
  } else {
    // A top-level function as a value: code that takes an environment and
    // calls it.
    llvm::Function *target = codegen_.functions_.lookup(fn->sym);
    code = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                                  llvm::GlobalValue::InternalLinkage, "tz." + fn->name + ".value",
                                  codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
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
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(utf16(fn->name))}),
      "tz.fninfo." + fn->name);
  infos_[fn] = global;

  return global;
}

llvm::Value *ClosureLowerer::emitFunctionExpr(FunctionExpr *fe) {
  FunctionDecl *fn = fe->fn;
  llvm::Constant *info = functionInfo(fn);
  const CodeGen::EnvLayout &layout = codegen_.envLowerer().envLayout(fn);
  // Without captures there is no environment, unless the program compares
  // functions: then each evaluation needs an identity of its own.
  llvm::Value *env = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  if (!layout.fields.empty() || (codegen_.comparesFunctions_ && !fn->nonEscaping)) {
    env = codegen_.envLowerer().createEnv(layout, fn->nonEscaping, !codegen_.unstoredSites_.contains(fe));
    codegen_.groupLowerer().noteUnstored(env, fe);
  }

  return makeFunctionValue(info, env);
}

llvm::Value *ClosureLowerer::closureEnv(CallExpr *call) {
  FunctionDecl *fn = call->func->decl;
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  if (codegen_.currentFn_ && (codegen_.currentFn_ == fn || (fn->group && codegen_.currentFn_->group == fn->group))) {
    return codegen_.envParam_ ? codegen_.envParam_ : null;
  }
  if (fn->group && fn->group->owner == codegen_.currentFn_) {
    return codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.groupEnvs_.lookup(fn->group));
  }
  // Through the variable that holds it, which cannot change.
  const VarSymbol *var = cast<NameExpr>(call->callee)->var;

  return codegen_.builder_.CreateExtractValue(codegen_.envLowerer().emitVarValue(var, /*borrow=*/true), 1);
}

llvm::Value *ClosureLowerer::callFunctionValue(llvm::Value *fn, const Type *fnType,
                                        llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Value *> all{codegen_.builder_.CreateExtractValue(fn, 1)};
  all.insert(all.end(), args.begin(), args.end());
  for (size_t i = args.size(); i < fnType->params().size(); ++i) {
    all.push_back(codegen_.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *info = codegen_.builder_.CreateExtractValue(fn, 0);
  llvm::Value *code = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(),
                                          codegen_.builder_.CreateStructGEP(codegen_.fnInfoTy_, info, 0), "code");
  llvm::CallInst *result = codegen_.builder_.CreateCall(closureFnType(fnType), code, all);

  return result->getType()->isVoidTy() ? nullptr : result;
}

// `f(x)` with a function value f: the function stays alive during the call.
// One in a field, `o.f(x)`, is not borrowed: the call may assign to the field.
llvm::Value *ClosureLowerer::emitValueCall(CallExpr *call) {
  // A method of an interface: the code the object's table gives.
  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    if (const CodeGen::InterfaceMember *method =
            codegen_.inheritLowerer().dispatchedMember(member->object->type, member->name, call->signature);
        (method && method->callable) || call->signature >= 0) {
      return codegen_.inheritLowerer().emitInterfaceCall(call);
    }
  }
  const Type *type = call->callee->type;
  CodeGen::Operand callee = codegen_.refCounts().emitOperand(call->callee, isa<NameExpr>(call->callee) &&
                                                 llvm::all_of(call->args, AssignLowerer::cannotAssign));
  codegen_.throwLowerer().holdOperand(callee, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(call->args);
  codegen_.throwLowerer().takeOperand(callee);
  llvm::Value *result = callFunctionValue(callee.value, type, args);
  codegen_.refCounts().dropOperand(callee);
  codegen_.emitThrowCheckIf(codegen_.typeThrows(type));

  return result;
}

// A function known by name, given a type that passes more or fewer
// arguments or drops the result: the same environment, with code that adapts
// the call.
llvm::Value *ClosureLowerer::emitFunctionAdapter(CastExpr *cast) {
  FunctionDecl *fn = knownFunction(cast->operand);
  assert(fn && "Sema adapts only functions known by name");
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = codegen_.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = adapters_[{fn, to}];
  if (!info) {
    auto *code = llvm::Function::Create(closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        qualifiedName(fn) + ".adapter", codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
    codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    llvm::ArrayRef<const Type *> params = from->params(), given = to->params();
    for (size_t i = 0; i < params.size(); ++i) {
      if (i >= given.size()) {
        args.push_back(codegen_.optionalLowerer().undefinedValue(params[i]));
        continue;
      }
      llvm::Value *arg = code->getArg(unsigned(i + 1));
      args.push_back(params[i] == given[i] ? arg
                                           : codegen_.optionalLowerer().makeOptional(arg, codegen_.builder_.getTrue(), params[i]));
    }
    // The arguments it does not take are released here: the callee owns them.
    for (size_t i = params.size(); i < given.size(); ++i) {
      codegen_.refCounts().releaseValue(code->getArg(unsigned(i + 1)), given[i]);
    }
    llvm::CallInst *result;
    if (fn->isClosure()) {
      args.insert(args.begin(), code->getArg(0));
      result = codegen_.builder_.CreateCall(closureCode(fn), args);
    } else {
      result = codegen_.builder_.CreateCall(codegen_.functions_.lookup(fn->sym), args);
    }
    if (to->ret()->isVoid()) {
      if (!from->ret()->isVoid()) {
        codegen_.refCounts().releaseValue(result, from->ret());
      }
      codegen_.builder_.CreateRetVoid();
    } else {
      codegen_.builder_.CreateRet(result);
    }
    codegen_.builder_.restoreIP(point);
    info = new llvm::GlobalVariable(
        *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(utf16(fn->name))}),
        "tz.fninfo." + fn->name);
  }

  // The environment's reference moves to the new value.
  return makeFunctionValue(info, codegen_.builder_.CreateExtractValue(value, 1));
}

void ClosureLowerer::emitClosure(FunctionDecl *fn) {
  if (fn->isGenerator) {
    codegen_.generatorLowerer().emitGenerator(fn, closureCode(fn));
    return;
  }
  if (fn->isAsync) {
    codegen_.asyncLowerer().emitAsyncFunction(fn, closureCode(fn));
    return;
  }
  codegen_.tailCallLowerer().findTailGroups(fn);
  if (CodeGen::TailGroup *group = codegen_.tailGroupOf_.lookup(fn)) {
    codegen_.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  codegen_.functionBuilder().beginFunction(closureCode(fn), fn);
  codegen_.envParam_ = codegen_.function_->getArg(0);
  codegen_.envLowerer().bindCaptures(fn);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < codegen_.function_->arg_size(); ++i) {
    args.push_back(codegen_.function_->getArg(i));
  }
  codegen_.functionBuilder().emitParamsAndBody(fn, args);
  codegen_.functionBuilder().finishFunction();
}

void ClosureLowerer::emitPendingClosures() {
  while (!codegen_.pendingClosures_.empty()) {
    FunctionDecl *fn = codegen_.pendingClosures_.back();
    codegen_.pendingClosures_.pop_back();
    emitClosure(fn);
  }
}

} // namespace tz
