#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class DictChecker;
class ExprChecker;
class JsonChecker;
class NameChecker;
class UnionChecker;
class UtilityTypes;
class MemberChecker;
class StaticChecker;
class TypeRelations;
class NestedFunctions;
class TopLevelChecker;
class KeyedFieldChecker;
class ClassMemberChecker;
class RuntimeObjectChecker;

class TargetChecker {
public:
  TargetChecker(SemanticState &owner, SemanticParts &all);

  VarSymbol *checkVarTarget(NameExpr *name);
  const Type *checkAssignTarget(Expr *&target, bool compound);

private:
  using ResultDefinedDictType = Result<const Type *, TargetCheckerDictDefinedError>;
  using ResultNoBytesProperty = Result<const Type *, TargetCheckerBytesMemberError>;
  using ResultTargetIsNotThis = Result<const NameExpr *, TargetCheckerNotThisError>;
  using ResultIndexIsNotBytes = Result<const Type *, TargetCheckerIndexNotBytesError>;
  using ResultObjectIsDefined = Result<const Type *, TargetCheckerObjectDefinedError>;
  using ResultIndexIsWritable = Result<const Type *, TargetCheckerIndexWritableError>;
  using ResultFieldIsWritable = Result<const Type *, TargetCheckerMemberWritableError>;
  using ResultVariableMutable = Result<const VarSymbol *, TargetCheckerVariableMutableError>;

  static ResultTargetIsNotThis notThis(const NameExpr *name);
  static ResultIndexIsNotBytes indexNotBytes(const Type *type);
  static ResultObjectIsDefined objectDefined(const Type *type);
  static ResultIndexIsWritable indexWritable(const Type *type);
  static ResultDefinedDictType dictDefined(const Type *type, const MemberExpr *member);
  static ResultNoBytesProperty bytesMember(const Type *type, const MemberExpr *member);
  static ResultVariableMutable variableMutable(const NameExpr *name, const VarSymbol *var);
  static ResultFieldIsWritable memberWritable(const Type *type, const MemberExpr *member,
                                              bool compound);

  SemanticState &state;
  Scopes *scopes = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  NameChecker *nameChecker = nullptr;
  UnionChecker *unionChecker = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  MemberChecker *memberChecker = nullptr;
  StaticChecker *staticChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  RuntimeObjectChecker *runtimeObjectChecker = nullptr;
};

}
