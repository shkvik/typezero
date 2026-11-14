#pragma once

#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class ClassCallRules {
public:
  explicit ClassCallRules(SemanticState &owner);

  void reportChoice(const VariantChoice &choice, llvm::SMLoc at);
  void pushPlainMethodCall(CallExpr *call, MemberExpr *member, FuncSymbol *function,
                           bool isChecked);
  void startGenericCall(CallExpr *call, FuncSymbol *function,
                        const std::vector<const Type *> &bound, llvm::SMLoc loc,
                        GenericCallee kind);


  CheckArguments argumentsTask(CallExpr *call, const FuncSymbol *function, llvm::SMLoc loc,
                               bool isChecked) const;

private:
  SemanticState &state;
};

}
