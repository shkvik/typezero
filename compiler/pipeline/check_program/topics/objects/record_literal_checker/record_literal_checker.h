#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/objects/record_literal_checker/record_literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class RecordLiteralChecker {
public:
  explicit RecordLiteralChecker(CheckState &owner);

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

  CheckState &state;
};

}
