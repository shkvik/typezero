#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class JsonChecker;
class LiteralTypes;
class ClassDeclarer;
class TypeRelations;
class KeyedFieldChecker;
class MethodLiteralChecker;
class RecordLiteralChecker;

class ObjectLiteralChecker {
public:
  ObjectLiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkObjectLit(ObjectLitExpr *literal, const Type *expected);

private:
  using ResultPropertyHasValue =
      Result<const ObjectLitExpr::Property *, ObjectLiteralCheckerPropertyPlainError>;
  using ResultComputedKeyKnown = Result<const Type *, ObjectLiteralCheckerComputedKeyError>;
  using ResultFieldNameIsFresh =
      Result<const ObjectLitExpr::Property *, ObjectLiteralCheckerUniqueNameError>;
  using ResultValueTypeIsKnown = Result<const Type *, ObjectLiteralCheckerInferredValueError>;
  using ResultKeyedValuesAgree = Result<const Type *, ObjectLiteralCheckerSameKeyedTypeError>;
  using ResultKeyWithoutSpread = Result<const Type *, ObjectLiteralCheckerKeyedPlacementError>;
  using ResultKeyedObjectTyped = Result<const Type *, ObjectLiteralCheckerKeyedTargetError>;
  using ResultKeyedFieldsAgree = Result<const Type *, ObjectLiteralCheckerKeyedFieldError>;
  using ResultTargetFieldFound = Result<const Field *, ObjectLiteralCheckerTargetFieldError>;
  using ResultUsableFieldValue = Result<const Type *, ObjectLiteralCheckerValueTypedError>;
  using ResultRequiredFieldSet = Result<const Field *, ObjectLiteralCheckerFieldGivenError>;
  using ResultDefinedSpreading = Result<const Type *, ObjectLiteralCheckerSpreadDefinedError>;
  using ResultSpreadIsAnObject = Result<const Type *, ObjectLiteralCheckerSpreadObjectError>;
  using ResultPlainSpreadClass = Result<const ClassDecl *, ObjectLiteralCheckerSpreadFieldsError>;
  using ResultSpreadFieldJoins = Result<const Type *, ObjectLiteralCheckerSpreadJoinError>;

  struct SpreadFields {
    const Type *target = nullptr;
    std::vector<Field> fields;
    bool hasRecords = false;
    bool isTyped = false;
  };

  struct ObjectLitShape {
    const Type *target;
    llvm::SmallVector<bool, 8> named;
    bool isTyped;
    bool isInferred;
  };

  ResultPropertyHasValue checkPropertyPlain(const ObjectLitExpr::Property &property) const;
  ResultComputedKeyKnown checkComputedKey(const Type *keyType) const;
  ResultFieldNameIsFresh checkUniqueName(const ObjectLitExpr *literal, size_t index,
                                         llvm::ArrayRef<bool> named) const;
  ResultValueTypeIsKnown checkInferredValue(const Type *value) const;
  ResultKeyedValuesAgree checkSameKeyedType(llvm::StringRef name, const Field &same,
                                            const Type *value) const;
  ResultKeyWithoutSpread checkKeyedPlacement(const ObjectLitExpr *literal,
                                             const Type *keyType) const;
  ResultKeyedObjectTyped checkKeyedTarget(const Type *target, const Type *keyType) const;
  ResultKeyedFieldsAgree checkKeyedField(const Type *target, const Type *keyType) const;
  ResultTargetFieldFound checkTargetField(const Type *target, llvm::StringRef name) const;
  ResultUsableFieldValue checkValueTyped(const Type *type, llvm::StringRef what,
                                         llvm::StringRef name) const;
  ResultRequiredFieldSet checkFieldGiven(const ObjectLitExpr *literal, const Field &field,
                                         const Type *target) const;
  bool hasPropertyNamed(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool isUsableTarget(const Type *type) const;
  void walkPropertyKey(ObjectLitExpr *literal, size_t index, ObjectLitShape &shape);
  void walkComputedKey(ObjectLitExpr::Property &property, ComputedProperty &computed, size_t index,
                       ObjectLitShape &shape);
  bool isLiteralKeys(const Type *keyType) const;
  void walkUniqueNames(ObjectLitExpr *literal, ObjectLitShape &shape);
  Expr *pickUnionTarget(ObjectLitExpr *literal, const Type *expectedUnion, ObjectLitShape &shape);
  void walkThisMethods(ObjectLitExpr *literal, const Type *target) const;
  bool isAllKeyed(const ObjectLitExpr *literal) const;
  bool hasMethod(const ObjectLitExpr *literal) const;
  Expr *checkInferredKeys(ObjectLitExpr *literal, ObjectLitShape &shape);
  bool reportInferredProperty(ObjectLitExpr::Property &property, std::vector<Field> &fields);
  bool reportKeyedFields(const ObjectLitExpr::Property &property, const Type *value,
                         std::vector<Field> &fields);
  bool reportKeyedClash(const ObjectLitExpr::Property &property, llvm::StringRef name,
                        const Field &same, const Type *value);
  const Field *fieldNamed(const std::vector<Field> &fields, llvm::StringRef name) const;
  Expr *checkKeyedProperties(ObjectLitExpr *literal, const ObjectLitShape &shape);
  Expr *checkKeyedProperty(ObjectLitExpr *literal, ObjectLitExpr::Property &property,
                           const ObjectLitShape &shape);
  std::vector<Field> checkNamedProperties(ObjectLitExpr *literal, ObjectLitShape &shape);
  void walkLiteralProperty(ObjectLitExpr::Property &property, bool isNamed, ObjectLitShape &shape,
                           std::vector<Field> &fields);
  void nameFunctionValue(ObjectLitExpr::Property &property) const;
  bool takePrechecked(const Expr *value);
  void walkTargetField(ObjectLitExpr::Property &property, const Type *target);
  void walkMissingField(ObjectLitExpr::Property &property,
                        const ObjectLiteralCheckerTargetFieldError &error, bool isPrechecked);
  Field untargetedField(ObjectLitExpr::Property &property, ObjectLitShape &shape);
  bool reportUnusableValue(const ObjectLitExpr::Property &property, const Type *type);
  Expr *finishObjectLit(ObjectLitExpr *literal, const ObjectLitShape &shape,
                        std::vector<Field> fields);
  ResultDefinedSpreading checkSpreadDefined(const Type *type) const;
  ResultSpreadIsAnObject checkSpreadObject(const Type *type) const;
  ResultPlainSpreadClass checkSpreadFields(const ClassDecl *classDecl) const;
  ResultSpreadFieldJoins checkSpreadJoin(const Field &spread, const Field &field,
                                         const Type *type) const;
  Expr *checkSpreadLit(ObjectLitExpr *literal, const Type *target, llvm::ArrayRef<bool> named,
                       bool isTyped);
  Field *findSlot(std::vector<Field> &fields, llvm::StringRef name) const;
  void walkNamedProp(ObjectLitExpr::Property &property, bool isNamed, SpreadFields &collected);
  void walkSpreadProp(ObjectLitExpr::Property &property, SpreadFields &collected);
  bool reportSpreadType(const ObjectLitExpr::Property &property, const Type *type);
  void takeOmitKeys(ObjectLitExpr::Property &property) const;
  bool reportSpreadClass(const ObjectLitExpr::Property &property, const Type *type);
  void mergeSpreadFields(const ObjectLitExpr::Property &property, const Type *type,
                         SpreadFields &collected);
  void mergeSpreadField(const ObjectLitExpr::Property &property, const Field &spreadField,
                        const Type *type, SpreadFields &collected);
  const Type *spreadResult(const SpreadFields &collected) const;
  bool isEveryFieldFitting(const std::vector<Field> &fields, const Type *target) const;
  bool isTargetFilled(const Type *target, const std::vector<Field> &fields) const;
  void convertSpreadProps(ObjectLitExpr *literal, const Type *result);
  void convertSpreadProp(ObjectLitExpr *literal, size_t index, const Type *result);
  void markReplacedFields(ObjectLitExpr *literal, size_t index) const;
  bool isReplacedLater(const ObjectLitExpr *literal, size_t index, llvm::StringRef name) const;
  bool isReplacing(const ObjectLitExpr::Property &later, llvm::StringRef name) const;

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  MethodLiteralChecker *methodLiteralChecker = nullptr;
  RecordLiteralChecker *recordLiteralChecker = nullptr;
};

}
