#include "compiler/semantic/code/functions/callback_arguments/callback_arguments.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CallbackArguments::CallbackArguments(SemanticState &owner, FunctionRules &sharedFunctionRules)
    : state(owner),
      functionRules(sharedFunctionRules) {}

void CallbackArguments::checkFunctionArgSlot(const CheckFunctionArgSlot &task) {
  Expr *arg = *task.slot;
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr != nullptr) {
    functionExpr->fn->nonEscaping = true;
    tasks.pushTask(AfterLiteralCallback{
        .slot = task.slot,
        .expected = task.expected,
        .method = task.method,
    });
    return tasks.pushTask(
        CheckFunctionExpr{.functionExpr = functionExpr, .expected = task.expected});
  }
  auto *wrapper =
      this->functionRules.builtinFunctionValue(arg, task.expected, task.primary, task.method);
  if (wrapper != nullptr) {
    wrapper->fn->nonEscaping = true;
    tasks.pushTask(WriteCheckedSlot{.slot = task.slot});
    return tasks.pushTask(CheckFunctionExpr{.functionExpr = wrapper, .expected = task.expected});
  }
  tasks.pushTask(AfterCallbackValue{
      .slot = task.slot,
      .expected = task.expected,
      .primary = task.primary,
      .method = task.method,
  });

  return this->startCallbackValue(arg, task.expected);
}

void CallbackArguments::afterLiteralCallback(const AfterLiteralCallback &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  *task.slot = checked;
  auto awaited = this->checkAsyncCallback(checked, task.expected, task.method);
  if (awaited.isErr()) {
    this->state.diagnostics.error(checked->loc(), awaited.error().message);
    checked->type = Type::error();
  }
}

void CallbackArguments::startCallbackValue(Expr *arg, const Signature &expected) {
  auto &tasks = this->state.walk.tasks;
  auto memberSignature =
      isa<MemberExpr>(arg) ? std::optional<Signature>(expected) : std::optional<Signature>();
  this->state.walk.functions.enterCallback(memberSignature);
  tasks.pushTask(LeaveCallback{});
  auto *generic = this->findGenericFunction(arg);
  if (generic != nullptr) {
    return tasks.pushTask(CheckGenericValue{
        .name = cast<NameExpr>(arg),
        .generic = generic,
        .wanted = expected,
    });
  }

  return tasks.pushTask(CheckExpr{.expr = arg, .expected = nullptr});
}

FuncSymbol *CallbackArguments::findGenericFunction(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);
  FuncSymbol *function = entry.func();
  if (function == nullptr || entry.var() != nullptr || !function->isGeneric()) {
    return nullptr;
  }

  return function;
}

void CallbackArguments::leaveCallback() {
  return this->state.walk.functions.leaveCallback();
}

void CallbackArguments::afterCallbackValue(const AfterCallbackValue &task) {
  Expr *value = this->state.walk.tasks.takeChecked();
  *task.slot = value;

  return this->checkPassedFunction(value, task.expected, task.primary, task.method);
}

void CallbackArguments::checkPassedFunction(Expr *arg, const Signature &expected, unsigned primary,
                                            llvm::StringRef method) {
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  auto given = this->checkFunctionGiven(type, method);
  if (given.isErr()) {
    return this->state.diagnostics.error(arg->loc(), given.error().message);
  }
  auto *known = this->namedFunction(arg);
  llvm::StringRef knownName = known != nullptr ? known->name : llvm::StringRef();
  auto name = ErrorTextFunctionName::quoted(knownName);
  auto fixed = this->checkFixedParams(type, name);
  if (fixed.isErr()) {
    return this->state.diagnostics.error(arg->loc(), fixed.error().message);
  }
  if (this->reportCallbackParams(arg, expected, primary, known, name, method)) {
    return;
  }
  auto enough = this->checkArgumentsEnough(type, expected, name, method);
  if (enough.isErr()) {
    return this->state.diagnostics.error(arg->loc(), enough.error().message);
  }

  return this->checkCallbackResult(arg, expected, name, method);
}

void CallbackArguments::checkCallbackResult(Expr *arg, const Signature &expected,
                                            llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  auto awaited = this->checkAsyncCallback(arg, expected, method);
  if (awaited.isErr()) {
    this->state.diagnostics.error(arg->loc(), awaited.error().message);
    arg->type = Type::error();
    return;
  }
  auto returns = this->checkReturnMatches(type, expected, name, method);
  if (returns.isErr()) {
    return this->state.diagnostics.error(arg->loc(), returns.error().message);
  }
  auto kept = this->checkResultKept(type, expected, name, method);
  if (kept.isErr()) {
    return this->state.diagnostics.error(arg->loc(), kept.error().message);
  }
}

CallbackArguments::ResultFunctionIsPassed
CallbackArguments::checkFunctionGiven(const Type *type, llvm::StringRef method) const {
  if (!type->isFunction()) {
    auto message = ErrorTextCallbackArg::notFunc(method, type->name());

    return err(CodeFunctionsRegistryFunctionGivenError{
        .code = CodeFunctionsRegistryFunctionGivenErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

CallbackArguments::ResultWithoutRestParam
CallbackArguments::checkFixedParams(const Type *type, llvm::StringRef name) const {
  if (type->hasRest()) {
    auto message = ErrorTextCallbackArg::rest(name);

    return err(CodeFunctionsRegistryFixedParamsError{
        .code = CodeFunctionsRegistryFixedParamsErrorCode::REST_PARAMETER,
        .message = message,
    });
  }

  return ok(type);
}

CallbackArguments::ResultParameterMatches
CallbackArguments::checkCallbackParam(const Type *type, const Signature &expected, size_t index,
                                      unsigned primary, const FunctionDecl *known,
                                      llvm::StringRef name, llvm::StringRef method) const {
  const Type *param = type->params()[index];
  const Type *given = expected.params[index];
  bool isFitting = param == given || (param->isOptional() && param->inner() == given);
  if (!isFitting) {
    auto position = ErrorTextCallbackArg::position(index);
    auto message =
        ErrorTextCallbackArg::paramType(name, param->name(), position, method, given->name());

    return err(CodeFunctionsRegistryCallbackParamError{
        .code = CodeFunctionsRegistryCallbackParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }
  if (index >= primary && index >= type->minArgs()) {
    bool isNamed = known != nullptr && index < known->params.size();
    std::string paramName =
        isNamed ? ErrorTextCallbackArg::paramName(known->params[index].name) : "";
    auto argName = ErrorTextCallbackArg::argName(method, index);
    auto args = ErrorTextCallbackArg::argsShape(method);
    auto message = ErrorTextCallbackArg::extraParam(name, argName, paramName, method, args);

    return err(CodeFunctionsRegistryCallbackParamError{
        .code = CodeFunctionsRegistryCallbackParamErrorCode::EXTRA_OPTIONAL,
        .message = message,
    });
  }

  return ok(param);
}

CallbackArguments::ResultArgumentsSuffice
CallbackArguments::checkArgumentsEnough(const Type *type, const Signature &expected,
                                        llvm::StringRef name, llvm::StringRef method) const {
  if (type->minArgs() > expected.params.size()) {
    unsigned count = unsigned(expected.params.size());
    auto passes = ErrorTextArity::wanted(count, count);
    std::string needs = std::to_string(type->minArgs());
    auto message = ErrorTextCallbackArg::tooFew(method, passes, name, needs);

    return err(CodeFunctionsRegistryArgumentsEnoughError{
        .code = CodeFunctionsRegistryArgumentsEnoughErrorCode::TOO_FEW_PASSED,
        .message = message,
    });
  }

  return ok(type);
}

CallbackArguments::ResultReturnedTypeFits
CallbackArguments::checkReturnMatches(const Type *type, const Signature &expected,
                                      llvm::StringRef name, llvm::StringRef method) const {
  if (expected.ret != nullptr && type->ret() != expected.ret && !expected.voidResult) {
    auto message =
        ErrorTextCallbackArg::badReturn(method, expected.ret->name(), name, type->ret()->name());

    return err(CodeFunctionsRegistryReturnMatchesError{
        .code = CodeFunctionsRegistryReturnMatchesErrorCode::WRONG_RETURN,
        .message = message,
    });
  }

  return ok(type);
}

CallbackArguments::ResultNothingIsDropped
CallbackArguments::checkResultKept(const Type *type, const Signature &expected,
                                   llvm::StringRef name, llvm::StringRef method) const {
  if (expected.voidResult && type->ret()->isResult()) {
    auto message = ErrorTextCallbackArg::noCheck(name, method);

    return err(CodeFunctionsRegistryResultKeptError{
        .code = CodeFunctionsRegistryResultKeptErrorCode::RESULT_DROPPED,
        .message = message,
    });
  }
  if (expected.voidResult && !expected.asyncVoid && type->ret()->isPromise()) {
    auto message = ErrorTextCallbackArg::noWait(name, method);

    return err(CodeFunctionsRegistryResultKeptError{
        .code = CodeFunctionsRegistryResultKeptErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(type);
}

bool CallbackArguments::reportCallbackParams(const Expr *arg, const Signature &expected,
                                             unsigned primary, const FunctionDecl *known,
                                             llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  size_t passed = std::min(type->params().size(), expected.params.size());
  for (size_t index = 0; index < passed; ++index) {
    auto param = this->checkCallbackParam(type, expected, index, primary, known, name, method);
    if (param.isErr()) {
      this->state.diagnostics.error(arg->loc(), param.error().message);
      return true;
    }
  }

  return false;
}

FunctionDecl *CallbackArguments::namedFunction(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  if (name->func() != nullptr) {
    return name->func()->decl;
  }

  return this->variableFunction(name->var());
}

FunctionDecl *CallbackArguments::variableFunction(const VarSymbol *variable) const {
  if (variable == nullptr) {
    return nullptr;
  }
  if (variable->function() != nullptr) {
    return variable->function();
  }
  if (variable->isConst) {
    return variable->lambda();
  }

  return nullptr;
}

CallbackArguments::ResultAsyncCallbackOk
CallbackArguments::checkAsyncCallback(const Expr *arg, const Signature &expected,
                                      llvm::StringRef method) const {
  const Type *type = arg->type;
  bool isPromised = type->isFunction() && type->ret()->isPromise() && expected.ret != nullptr &&
                    !expected.ret->isPromise() && !expected.voidResult && !expected.ret->isError();
  const auto *stringMethod = StringMethodTable::find(method);
  bool isReplacing = stringMethod != nullptr && (stringMethod->method == StringMethod::REPLACE ||
                                                 stringMethod->method == StringMethod::REPLACE_ALL);
  if (isPromised && isReplacing) {
    auto message = ErrorTextAsyncCallback::replace(method, expected.ret->name());

    return err(CodeFunctionsRegistryAsyncCallbackError{
        .code = CodeFunctionsRegistryAsyncCallbackErrorCode::REPLACE_GETS_PROMISE,
        .message = message,
    });
  }
  if (isPromised && expected.ret->isBool()) {
    auto message = ErrorTextAsyncCallback::test(method, expected.ret->name());

    return err(CodeFunctionsRegistryAsyncCallbackError{
        .code = CodeFunctionsRegistryAsyncCallbackErrorCode::TEST_GETS_PROMISE,
        .message = message,
    });
  }
  if (isPromised) {
    auto message = ErrorTextAsyncCallback::value(method, expected.ret->name());

    return err(CodeFunctionsRegistryAsyncCallbackError{
        .code = CodeFunctionsRegistryAsyncCallbackErrorCode::VALUE_GETS_PROMISE,
        .message = message,
    });
  }

  return ok(arg);
}

}
