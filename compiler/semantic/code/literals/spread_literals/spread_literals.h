#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/code/literals/literal_rules/literal_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class SpreadLiterals {
public:
  SpreadLiterals(SemanticState &owner, LiteralRules &sharedLiteralRules);

  void afterNamedProp(const AfterNamedProp &task);
  void afterSpreadProp(const AfterSpreadProp &task);
  void afterRecordSpread(const AfterRecordSpread &task);
  void nextRecordProperty(const NextRecordProperty &task);
  void nextSpreadProperty(const NextSpreadProperty &task);

private:
  struct ValueCandidate {
    const Type *type;
    llvm::SMLoc loc;
  };
  struct RecordValue {
    const Type *value;
    bool isTyped;
  };

  using ResultValueTypeJoin = Result<const Type *, CodeLiteralsRegistryValuesJoinError>;
  using ResultValueTypeKnow = Result<const Type *, CodeLiteralsRegistryValueKnownError>;
  using ResultSpreadFieldJoins = Result<const Type *, CodeLiteralsRegistrySpreadJoinError>;
  using ResultSpreadIsAnObject = Result<const Type *, CodeLiteralsRegistrySpreadObjectError>;
  using ResultDefinedSpreading = Result<const Type *, CodeLiteralsRegistrySpreadDefinedError>;
  using ResultSpreadIntoMap = Result<const Type *, CodeLiteralsRegistryCheckRecordSpreadError>;
  using ResultPlainSpreadClass = Result<const ClassDecl *, CodeLiteralsRegistrySpreadFieldsError>;
  using ResultPlainProperty =
      Result<const ObjectLitExpr::Property *, CodeLiteralsRegistryPlainPropertyError>;

  void finishSpreadLit(const SpreadDraft &draft);
  void recordFromSpreads(ObjectLitExpr *literal);
  void takeOmitKeys(ObjectLitExpr::Property &property) const;
  void markReplacedFields(ObjectLitExpr *literal, size_t index) const;
  void convertSpreadProp(ObjectLitExpr *literal, size_t index, const Type *result);
  void mergeSpreadFields(const ObjectLitExpr::Property &property, const Type *type,
                         SpreadDraft &collected);
  void addFieldCandidates(const ObjectLitExpr::Property &property,
                          std::vector<ValueCandidate> &candidates) const;
  void reportSpread(const ObjectLitExpr::Property &property,
                    const CodeLiteralsRegistryCheckRecordSpreadError &error);
  void mergeSpreadField(const ObjectLitExpr::Property &property, const Field &spreadField,
                        const Type *type, SpreadDraft &collected);


  bool hasClassMethods(const Type *type) const;
  bool isMethodField(const ClassField &field) const;
  bool isTargetFilled(const Type *target, const std::vector<Field> &fields) const;
  bool reportSpreadType(const ObjectLitExpr::Property &property, const Type *type);
  bool reportSpreadClass(const ObjectLitExpr::Property &property, const Type *type);
  bool reportSpreadIntoRecord(ObjectLitExpr::Property &property, const Type *value);
  bool isReplacing(const ObjectLitExpr::Property &later, llvm::StringRef name) const;
  bool isEveryFieldFitting(const std::vector<Field> &fields, const Type *target) const;
  bool isReplacedLater(const ObjectLitExpr *literal, size_t index, llvm::StringRef name) const;


  RecordValue joinValue(const RecordValue &current, const ValueCandidate &candidate);
  llvm::SMLoc spreadErrorPlace(const ObjectLitExpr::Property &property,
                               CodeLiteralsRegistryCheckRecordSpreadErrorCode code) const;


  ResultValueTypeKnow checkValueKnown(const Type *value) const;
  ResultValueTypeJoin checkValuesJoin(const Type *value, const Type *type) const;
  ResultSpreadIntoMap checkRecordValues(const Type *type, const Type *value) const;
  ResultSpreadIntoMap checkSpreadSource(const ObjectLitExpr::Property &property) const;
  ResultPlainProperty checkPlainProperty(const ObjectLitExpr::Property &property) const;
  ResultSpreadIntoMap checkRecordSpread(ObjectLitExpr::Property &property, const Type *value);
  ResultSpreadIntoMap checkObjectSpread(const ObjectLitExpr::Property &property, const Type *value);
  ResultSpreadIntoMap checkRecordSpreadFields(const ObjectLitExpr::Property &property,
                                              const Type *value) const;


  ResultSpreadIsAnObject checkSpreadObject(const Type *type) const;
  ResultDefinedSpreading checkSpreadDefined(const Type *type) const;
  ResultPlainSpreadClass checkSpreadFields(const ClassDecl *classDecl) const;
  ResultSpreadFieldJoins checkSpreadJoin(const Field &spread, const Field &field,
                                         const Type *type) const;


  const Type *spreadResult(const SpreadDraft &collected) const;
  Field *findSlot(std::vector<Field> &fields, llvm::StringRef name) const;
  std::vector<ValueCandidate> valueCandidates(const ObjectLitExpr *literal) const;

  SemanticState &state;
  LiteralRules &literalRules;
};

}
