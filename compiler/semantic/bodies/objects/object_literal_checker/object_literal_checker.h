#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ObjectLiteralChecker {
public:
  explicit ObjectLiteralChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
