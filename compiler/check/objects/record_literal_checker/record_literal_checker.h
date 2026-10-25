#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Literals of Record and their spreads.
class RecordLiteralChecker {
public:
  explicit RecordLiteralChecker(Sema &sema);

  // An object literal where a Record is expected, and one whose spreads
  // make it a Record (`{ ...a, ...b }` of Records).
  Expr *checkRecordLit(ObjectLitExpr *lit, const Type *record);
  bool checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value);
  Expr *recordFromSpreads(ObjectLitExpr *lit);

private:
  Sema &sema_;
};

} // namespace tz
