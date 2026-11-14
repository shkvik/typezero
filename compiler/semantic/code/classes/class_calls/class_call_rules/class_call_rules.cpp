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
  tasks.pushTask(StartGenericCallTypes{
      .call = call,
      .function = function,
      .bound = bound,
      .loc = loc,
      .kind = kind,
  });

  return tasks.pushTask(ReturnTypeOf{.function = function});
}

}
