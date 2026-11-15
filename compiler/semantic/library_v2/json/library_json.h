#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include <optional>
#include <string>
#include <utility>

namespace tz {

class SemanticState;

class LibraryJson {
public:
  explicit LibraryJson(SemanticState &owner);

  void addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;


  bool isNullName(const Expr *expr) const;

private:
  void addArrayFacts(const Expr *arg, Flow &whenTrue) const;
  void narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) const;
  void addSideFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  void addNothingFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  void addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                      Flow &different) const;


  bool isNothing(const Expr *expr) const;
  bool isNarrowedPath(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;


  const Type *typeOfJson(const std::u16string &text) const;
  const VarSymbol *jsonVariable(const Expr *expr) const;
  std::optional<FieldPath> jsonField(const Expr *expr) const;

  SemanticState &state;
};

}
