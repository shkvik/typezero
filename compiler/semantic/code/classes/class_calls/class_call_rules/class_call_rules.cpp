#include "compiler/semantic/code/classes/class_calls/class_call_rules/class_call_rules.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ClassCallRules::ClassCallRules(SemanticState &owner)
    : state(owner) {}

void ClassCallRules::reportChoice(const VariantChoice &choice, llvm::SMLoc at) {
  if (!choice.isReported) {
    return this->state.diagnostics.error(at, choice.message);
  }
}

void ClassCallRules::pushPlainMethodCall(CallExpr *call, MemberExpr *member, FuncSymbol *function,
                                         bool isChecked) {
  auto &tasks = this->state.walk.tasks;
  auto arguments = this->argumentsTask(call, function, member->loc(), isChecked);
  tasks.pushTask(std::move(arguments));
  tasks.pushTask(AfterPlainMethodType{.call = call, .member = member, .function = function});

  return tasks.pushTask(FunctionTypeOf{.function = function});
}

CheckArguments ClassCallRules::argumentsTask(CallExpr *call, const FuncSymbol *function,
                                             llvm::SMLoc loc, bool isChecked) const {
  size_t checkedCount = isChecked ? call->args.size() : 0;

  return CheckArguments{
      .call = call,
      .params = function->params,
      .minArgs = function->minArgs,
      .hasRest = function->hasRest,
      .loc = loc,
      .checked = std::vector<bool>(checkedCount, true),
  };
}

void ClassCallRules::startGenericCall(CallExpr *call, FuncSymbol *function,
                                      const std::vector<const Type *> &bound, llvm::SMLoc loc,
                                      GenericCallee kind) {
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = function->getReturnType();
  this->state.generics.copies.enterInstanceLoc(loc);
  tasks.pushTask(AfterBoundTypes{
      .call = call,
      .function = function,
      .bound = bound,
      .loc = loc,
      .kind = kind,
      .isReturnKnown = returnType != nullptr,
  });
  if (returnType != nullptr) {
    tasks.pushTask(
        SubstituteType{.type = returnType, .params = function->typeParams, .args = bound});
  }
  for (const Type *param : llvm::reverse(function->params)) {
    tasks.pushTask(
        SubstituteValueType{.type = param, .params = function->typeParams, .args = bound});
  }
}

}
