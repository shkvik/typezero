#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/instances/constructor_calls/constructor_calls_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ConstructorCalls {
public:
  explicit ConstructorCalls(CheckState &owner);

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

  CheckState &state;
};

}
