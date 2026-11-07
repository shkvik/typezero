#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/objects/record_literal_checker/record_literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class RecordLiteralChecker {
public:
  explicit RecordLiteralChecker(Checker &owner);

  Expr *checkRecordLit(ObjectLitExpr *lit, const Type *record);
  Expr *recordFromSpreads(ObjectLitExpr *lit);

private:
  using ResultPlainProperty =
      Result<const ObjectLitExpr::Property *, RecordLiteralCheckerPlainPropertyError>;
  using ResultSpreadIntoMap = Result<const Type *, RecordLiteralCheckerCheckRecordSpreadError>;
  using ResultValueTypeJoin = Result<const Type *, RecordLiteralCheckerValuesJoinError>;
  using ResultValueTypeKnow = Result<const Type *, RecordLiteralCheckerValueKnownError>;

  static ResultPlainProperty plainProperty(const ObjectLitExpr::Property &prop);
  static ResultValueTypeJoin valuesJoin(const Type *value, const Type *type);
  static ResultValueTypeKnow valueKnown(const Type *value);
  ResultSpreadIntoMap checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value);
  void reportSpread(const ObjectLitExpr::Property &prop, const ResultSpreadIntoMap &spread);

  Checker &checker;
};

}
