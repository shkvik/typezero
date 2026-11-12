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

  struct ValueCandidate {
    const Type *type;
    llvm::SMLoc loc;
  };

  struct RecordValue {
    const Type *value;
    bool isTyped;
  };

  ResultPlainProperty checkPlainProperty(const ObjectLitExpr::Property &property) const;
  void reportSpread(const ObjectLitExpr::Property &property,
                    const RecordLiteralCheckerCheckRecordSpreadError &error);
  llvm::SMLoc spreadErrorPlace(const ObjectLitExpr::Property &property,
                               RecordLiteralCheckerCheckRecordSpreadErrorCode code) const;
  bool reportRecordProperty(ObjectLitExpr::Property &property, const Type *record);
  bool reportRecordSpread(ObjectLitExpr::Property &property, const Type *record);
  bool reportSpreadIntoRecord(ObjectLitExpr::Property &property, const Type *value);
  void walkRecordValue(ObjectLitExpr::Property &property, const Type *value);
  ResultSpreadIntoMap checkRecordSpread(ObjectLitExpr::Property &property, const Type *value);
  ResultSpreadIntoMap checkSpreadSource(const ObjectLitExpr::Property &property) const;
  ResultSpreadIntoMap checkRecordValues(const Type *type, const Type *value) const;
  ResultSpreadIntoMap checkObjectSpread(const ObjectLitExpr::Property &property, const Type *value);
  ResultSpreadIntoMap checkSpreadFields(const ObjectLitExpr::Property &property,
                                        const Type *value) const;
  bool hasClassMethods(const Type *type) const;
  bool isMethodField(const ClassField &field) const;
  ResultValueTypeJoin checkValuesJoin(const Type *value, const Type *type) const;
  ResultValueTypeKnow checkValueKnown(const Type *value) const;
  std::vector<ValueCandidate> valueCandidates(const ObjectLitExpr *literal) const;
  void addFieldCandidates(const ObjectLitExpr::Property &property,
                          std::vector<ValueCandidate> &candidates) const;
  RecordValue joinValue(const RecordValue &current, const ValueCandidate &candidate);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
