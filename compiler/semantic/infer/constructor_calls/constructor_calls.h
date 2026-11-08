#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/infer/constructor_calls/constructor_calls_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ConstructorCalls {
public:
  explicit ConstructorCalls(SemanticState &owner);

  void checkConstructorCalls();

private:
  using ResultNotOverridden = Result<FunctionDecl *, ConstructorCallsOverriddenCallError>;

  static const NameExpr *receiverName(const CallExpr *call);
  static const ClassDecl *overridingChild(const ClassDecl *cls, const FunctionDecl *method);
  static ResultNotOverridden overriddenCall(const ClassDecl *cls, const FunctionDecl *ctor,
                                            const CallExpr *call, const CallExpr *site);

  void scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls, const CallExpr *origin,
                            const FunctionDecl *ctor,
                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);

  SemanticState &state;
};

}
