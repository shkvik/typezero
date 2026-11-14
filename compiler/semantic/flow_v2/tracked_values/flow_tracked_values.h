#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowTrackedValues {
public:
  explicit FlowTrackedValues(SemanticState &owner);

  void defineField(Flow &flow, const FieldPath &path) const;


  const Type *narrowedFieldType(const MemberExpr *member) const;
  const Type *typeIn(const Flow &flow, const VarSymbol *variable) const;


  std::optional<FieldPath> fieldPath(const Expr *expr) const;

private:
  bool isHoldingField(const MemberExpr *member) const;


  const Type *streamView(const MemberExpr *member) const;

  SemanticState &state;
};

}
