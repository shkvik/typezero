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

  Expr *checkObjectLit(ObjectLitExpr *lit, const Type *expected);

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

  static bool hasMethodFields(const ClassDecl *cls);
  static ResultSpreadIsAnObject spreadObject(const Type *type);
  static ResultDefinedSpreading spreadDefined(const Type *type);
  static ResultComputedKeyKnown computedKey(const Type *keyType);
  static ResultValueTypeIsKnown inferredValue(const Type *value);
  static ResultPlainSpreadClass spreadFields(const ClassDecl *cls);
  static ResultKeyedObjectTyped keyedTarget(const Type *target, const Type *keyType);
  static ResultKeyedFieldsAgree keyedField(const Type *target, const Type *keyType);
  static ResultKeyWithoutSpread keyedPlacement(const ObjectLitExpr *lit, const Type *keyType);
  static ResultTargetFieldFound targetField(const Type *target, llvm::StringRef name);
  static ResultPropertyHasValue propertyPlain(const ObjectLitExpr::Property &prop);
  static ResultRequiredFieldSet fieldGiven(const ObjectLitExpr *lit, const Field &field,
                                           const Type *target);
  static ResultUsableFieldValue valueTyped(const Type *type, llvm::StringRef what,
                                           llvm::StringRef name);
  static ResultKeyedValuesAgree sameKeyedType(llvm::StringRef name, const Field &same,
                                              const Type *value);
  static ResultFieldNameIsFresh uniqueName(const ObjectLitExpr *lit, size_t i,
                                           llvm::ArrayRef<bool> named);
  static ResultSpreadFieldJoins spreadJoin(const Field &spread, const Field &field,
                                           const Type *type);
  Expr *checkSpreadLit(ObjectLitExpr *lit, const Type *target, llvm::ArrayRef<bool> named,
                       bool typed);

  struct SpreadFields {
    const Type *target = nullptr;
    std::vector<Field> fields;
    bool records = false;
    bool typed = false;
  };

  static Field *findSlot(std::vector<Field> &fields, llvm::StringRef name);
  void walkNamedProp(ObjectLitExpr::Property &prop, bool isNamed, SpreadFields &collected);
  void walkSpreadProp(ObjectLitExpr::Property &prop, SpreadFields &collected);
  bool spreadTypeFits(const ObjectLitExpr::Property &prop, const Type *type);
  static void takeOmitKeys(ObjectLitExpr::Property &prop);
  bool spreadClassFits(const ObjectLitExpr::Property &prop, const Type *type);
  void mergeSpreadFields(const ObjectLitExpr::Property &prop, const Type *type,
                         SpreadFields &collected);
  static const Type *spreadResult(const SpreadFields &collected);
  static bool fieldsFitTarget(const std::vector<Field> &fields, const Type *target);
  static bool targetFilled(const Type *target, const std::vector<Field> &fields);
  void convertSpreadProps(ObjectLitExpr *lit, const Type *result);
  static void markReplacedFields(ObjectLitExpr *lit, size_t i);
  static bool isReplacedLater(const ObjectLitExpr *lit, size_t i, llvm::StringRef name);

  struct ObjectLitShape {
    const Type *target;
    llvm::SmallVector<bool, 8> named;
    bool typed;
    bool inferred;
  };

  static bool isUsableTarget(const Type *t);
  void walkPropertyKey(ObjectLitExpr *lit, size_t i, ObjectLitShape &shape);
  static bool isLiteralKeys(const Type *keyType);
  void walkUniqueNames(ObjectLitExpr *lit, ObjectLitShape &shape);
  Expr *pickUnionTarget(ObjectLitExpr *lit, const Type *expectedUnion, ObjectLitShape &shape);
  static void walkThisMethods(ObjectLitExpr *lit, const Type *target);
  static bool isAllKeyed(const ObjectLitExpr *lit);
  static bool hasMethod(const ObjectLitExpr *lit);
  Expr *checkInferredKeys(ObjectLitExpr *lit, ObjectLitShape &shape);
  bool keyedFieldsAgree(const ObjectLitExpr::Property &prop, const Type *value,
                        std::vector<Field> &fields);
  static const Field *fieldNamed(const std::vector<Field> &fields, llvm::StringRef name);
  Expr *checkKeyedProperties(ObjectLitExpr *lit, const ObjectLitShape &shape);
  Expr *checkKeyedProperty(ObjectLitExpr *lit, ObjectLitExpr::Property &prop,
                           const ObjectLitShape &shape);
  std::vector<Field> checkNamedProperties(ObjectLitExpr *lit, ObjectLitShape &shape);
  bool takePrechecked(const Expr *value);
  void walkTargetField(ObjectLitExpr::Property &prop, const Type *target);
  Field untargetedField(ObjectLitExpr::Property &prop, ObjectLitShape &shape);
  Expr *finishObjectLit(ObjectLitExpr *lit, const ObjectLitShape &shape, std::vector<Field> fields);

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
