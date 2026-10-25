#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Literals of Record and their spreads.
class RecordLiteralChecker {
public:
  explicit RecordLiteralChecker(Checker &checker);

  // An object literal where a Record is expected, and one whose spreads
  // make it a Record (`{ ...a, ...b }` of Records).
  Expr *checkRecordLit(ObjectLitExpr *lit, const Type *record);
  bool checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value);
  Expr *recordFromSpreads(ObjectLitExpr *lit);

private:
  Checker &checker_;
};

} // namespace tz
