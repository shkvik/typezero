#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Which optional fields are known to be set.
class FieldFacts {
public:
  explicit FieldFacts(Checker &checker);

  // Fields that hold a value (see Flow::definedFields). A narrowed field is
  // forgotten when any field of that name is assigned to, the variable it
  // starts from changes, or a function is called and some code of the
  // program assigns to a field of that name: any of those may change it.
  std::optional<Checker::FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const Checker::FieldPath &path) const;
  static void defineField(Checker::Flow &flow, const Checker::FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Checker::Flow &flow, const VarSymbol *var);
  // `o.shape` narrowed by a check: as its narrowed type; or `member` itself.
  Expr *narrowedField(MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
