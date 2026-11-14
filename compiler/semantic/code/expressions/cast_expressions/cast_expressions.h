#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/expressions/code_expressions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class CastExpressions {
public:
  explicit CastExpressions(SemanticState &owner);

  void finishCast(const FinishCast &task);
  void afterCastTarget(const AfterCastTarget &task);

private:
  using ResultCastSupported = Result<const CastExpr *, CodeExpressionsRegistryCheckCastError>;

  std::string hintedCastMessage(const Expr *operand, const Type *to) const;
  std::string castMessage(CodeExpressionsRegistryCheckCastErrorCode code, const Expr *operand,
                          const Type *to) const;


  ResultCastSupported checkCast(const CastExpr *castExpr, bool isArrayLiteral) const;
  CodeExpressionsRegistryCheckCastErrorCode castProblem(const Expr *operand, const Type *to,
                                                        bool isArrayLiteral) const;

  SemanticState &state;
};

}
