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

  bool hasMethodFields(const ClassDecl *classDecl) const;
  ResultSpreadIsAnObject spreadObject(const Type *type) const;
  ResultDefinedSpreading spreadDefined(const Type *type) const;
  ResultComputedKeyKnown computedKey(const Type *keyType) const;
  ResultValueTypeIsKnown inferredValue(const Type *value) const;
  ResultPlainSpreadClass spreadFields(const ClassDecl *classDecl) const;
  ResultKeyedObjectTyped keyedTarget(const Type *target, const Type *keyType) const;
  ResultKeyedFieldsAgree keyedField(const Type *target, const Type *keyType) const;
  ResultKeyWithoutSpread keyedPlacement(const ObjectLitExpr *literal, const Type *keyType) const;
  ResultTargetFieldFound targetField(const Type *target, llvm::StringRef name) const;
  ResultPropertyHasValue propertyPlain(const ObjectLitExpr::Property &property) const;
  ResultRequiredFieldSet fieldGiven(const ObjectLitExpr *literal, const Field &field,
                                    const Type *target) const;
  ResultUsableFieldValue valueTyped(const Type *type, llvm::StringRef what,
                                    llvm::StringRef name) const;
  ResultKeyedValuesAgree sameKeyedType(llvm::StringRef name, const Field &same,
                                       const Type *value) const;
  ResultFieldNameIsFresh uniqueName(const ObjectLitExpr *literal, size_t index,
                                    llvm::ArrayRef<bool> named) const;
  ResultSpreadFieldJoins spreadJoin(const Field &spread, const Field &field,
                                    const Type *type) const;
  Expr *checkSpreadLit(ObjectLitExpr *literal, const Type *target, llvm::ArrayRef<bool> named,
                       bool typed);

  struct SpreadFields {
    const Type *target = nullptr;
    std::vector<Field> fields;
    bool records = false;
    bool typed = false;
  };

  Field *findSlot(std::vector<Field> &fields, llvm::StringRef name) const;
  void walkNamedProp(ObjectLitExpr::Property &property, bool isNamed, SpreadFields &collected);
  void walkSpreadProp(ObjectLitExpr::Property &property, SpreadFields &collected);
  bool spreadTypeFits(const ObjectLitExpr::Property &property, const Type *type);
  void takeOmitKeys(ObjectLitExpr::Property &property) const;
  bool spreadClassFits(const ObjectLitExpr::Property &property, const Type *type);
  void mergeSpreadFields(const ObjectLitExpr::Property &property, const Type *type,
                         SpreadFields &collected);
  const Type *spreadResult(const SpreadFields &collected) const;
  bool fieldsFitTarget(const std::vector<Field> &fields, const Type *target) const;
  bool targetFilled(const Type *target, const std::vector<Field> &fields) const;
  void convertSpreadProps(ObjectLitExpr *literal, const Type *result);
  void markReplacedFields(ObjectLitExpr *literal, size_t index) const;
  bool isReplacedLater(const ObjectLitExpr *literal, size_t index, llvm::StringRef name) const;

  struct ObjectLitShape {
    const Type *target;
    llvm::SmallVector<bool, 8> named;
    bool typed;
    bool inferred;
  };

  bool isUsableTarget(const Type *type) const;
  void walkPropertyKey(ObjectLitExpr *literal, size_t index, ObjectLitShape &shape);
  bool isLiteralKeys(const Type *keyType) const;
  void walkUniqueNames(ObjectLitExpr *literal, ObjectLitShape &shape);
  Expr *pickUnionTarget(ObjectLitExpr *literal, const Type *expectedUnion, ObjectLitShape &shape);
  void walkThisMethods(ObjectLitExpr *literal, const Type *target) const;
  bool isAllKeyed(const ObjectLitExpr *literal) const;
  bool hasMethod(const ObjectLitExpr *literal) const;
  Expr *checkInferredKeys(ObjectLitExpr *literal, ObjectLitShape &shape);
  bool keyedFieldsAgree(const ObjectLitExpr::Property &property, const Type *value,
                        std::vector<Field> &fields);
  const Field *fieldNamed(const std::vector<Field> &fields, llvm::StringRef name) const;
  Expr *checkKeyedProperties(ObjectLitExpr *literal, const ObjectLitShape &shape);
  Expr *checkKeyedProperty(ObjectLitExpr *literal, ObjectLitExpr::Property &property,
                           const ObjectLitShape &shape);
  std::vector<Field> checkNamedProperties(ObjectLitExpr *literal, ObjectLitShape &shape);
  bool takePrechecked(const Expr *value);
  void walkTargetField(ObjectLitExpr::Property &property, const Type *target);
  Field untargetedField(ObjectLitExpr::Property &property, ObjectLitShape &shape);
  Expr *finishObjectLit(ObjectLitExpr *literal, const ObjectLitShape &shape,
                        std::vector<Field> fields);

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
