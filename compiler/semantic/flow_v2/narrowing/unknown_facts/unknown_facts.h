#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class UnknownFacts {
public:
  explicit UnknownFacts(SemanticState &owner);

  void addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  void narrowByTypeOf(const BinaryExpr *binary, Flow &equal) const;
  void narrowByInstanceOf(const BinaryExpr *binary, Flow &whenTrue) const;
  void narrowChecked(const Expr *checked, const Type *type, Flow &flow) const;
  void narrowTypeOfSide(const Expr *side, const Expr *other, Flow &equal) const;
  void replaceNarrowing(Flow &flow, const VarSymbol *variable, const Type *type) const;


  const Type *instanceType(const Type *type) const;
  const VarSymbol *checkedVariable(const Expr *expr) const;

  SemanticState &state;
};

}
