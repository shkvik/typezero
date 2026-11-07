#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class FieldFacts {
public:
  explicit FieldFacts(CheckState &owner);

  std::optional<FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const FieldPath &path) const;
  static void defineField(Flow &flow, const FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Flow &flow, const VarSymbol *var);
  Expr *narrowedField(MemberExpr *member);

private:
  CheckState &state;
};

}
