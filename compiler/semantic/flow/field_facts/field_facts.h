#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FieldFacts {
public:
  explicit FieldFacts(SemanticState &owner);

  std::optional<FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const FieldPath &path) const;
  static void defineField(Flow &flow, const FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Flow &flow, const VarSymbol *var);
  Expr *narrowedField(MemberExpr *member);

private:
  SemanticState &state;
};

}
