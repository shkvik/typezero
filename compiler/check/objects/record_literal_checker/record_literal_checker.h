#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class RecordLiteralChecker {
public:
  explicit RecordLiteralChecker(Checker &checker);

  Expr *checkRecordLit(ObjectLitExpr *lit, const Type *record);
  bool checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value);
  Expr *recordFromSpreads(ObjectLitExpr *lit);

private:
  Checker &checker_;
};

}
