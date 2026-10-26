#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FieldFacts {
public:
  explicit FieldFacts(Checker &checker);

  std::optional<Checker::FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const Checker::FieldPath &path) const;
  static void defineField(Checker::Flow &flow, const Checker::FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Checker::Flow &flow, const VarSymbol *var);
  Expr *narrowedField(MemberExpr *member);

private:
  Checker &checker_;
};

}
