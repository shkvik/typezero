#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowDefinedValues {
public:
  explicit FlowDefinedValues(SemanticState &owner);

  void defineChain(const Expr *expr, Flow &flow) const;
  void defineOptional(const Expr *expr, Flow &flow) const;
  void addChainFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addConvertedFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addUndefinedFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;


  bool isOptionalChain(const Expr *expr) const;


  const NameExpr *optionalVariable(const Expr *expr) const;
  std::optional<FieldPath> optionalField(const Expr *expr) const;

private:
  void extendPath(const Expr *link, FieldPath &path, Flow &flow) const;
  void addSideConvertedFacts(const Expr *side, const Expr *other, Flow &whenEqual) const;
  void addSideChainFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                         Flow &whenUnequal) const;


  bool isDefinedValue(const Type *type) const;


  const Expr *pathInner(const Expr *expr) const;
  const Expr *withoutPlainCasts(const Expr *expr) const;


  std::optional<FieldPath> pathRoot(const Expr *expr) const;
  std::optional<FieldPath> definedPath(const Expr *expr, Flow &flow) const;

  SemanticState &state;
};

}
