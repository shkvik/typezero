#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Which optional fields are known to be set.
class FieldFacts {
public:
  explicit FieldFacts(Sema &sema);

  // Fields that hold a value (see Flow::definedFields). A narrowed field is
  // forgotten when any field of that name is assigned to, the variable it
  // starts from changes, or a function is called and some code of the
  // program assigns to a field of that name: any of those may change it.
  std::optional<Sema::FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const Sema::FieldPath &path) const;
  static void defineField(Sema::Flow &flow, const Sema::FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Sema::Flow &flow, const VarSymbol *var);
  // `o.shape` narrowed by a check: as its narrowed type; or `member` itself.
  Expr *narrowedField(MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
