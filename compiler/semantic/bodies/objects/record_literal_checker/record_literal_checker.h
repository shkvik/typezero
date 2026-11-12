#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/record_literal_checker/record_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class DictChecker;
class ExprChecker;
class ClassDeclarer;
class TypeRelations;

class RecordLiteralChecker {
public:
  RecordLiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkRecordLit(ObjectLitExpr *literal, const Type *record);
  Expr *recordFromSpreads(ObjectLitExpr *literal);

private:
  using ResultPlainProperty =
      Result<const ObjectLitExpr::Property *, RecordLiteralCheckerPlainPropertyError>;
  using ResultSpreadIntoMap = Result<const Type *, RecordLiteralCheckerCheckRecordSpreadError>;
  using ResultValueTypeJoin = Result<const Type *, RecordLiteralCheckerValuesJoinError>;
  using ResultValueTypeKnow = Result<const Type *, RecordLiteralCheckerValueKnownError>;

  ResultPlainProperty plainProperty(const ObjectLitExpr::Property &property) const;
  ResultValueTypeJoin valuesJoin(const Type *value, const Type *type) const;
  ResultValueTypeKnow valueKnown(const Type *value) const;
  ResultSpreadIntoMap checkRecordSpread(ObjectLitExpr::Property &property, const Type *value);
  void reportSpread(const ObjectLitExpr::Property &property, const ResultSpreadIntoMap &spread);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
