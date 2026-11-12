#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_errors.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_types.h"
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
  const Type *checkAssignTarget(Expr *&target, AssignForm form);

private:
  using ResultDefinedDictType = Result<const Type *, TargetCheckerDictDefinedError>;
  using ResultNoBytesProperty = Result<const Type *, TargetCheckerBytesMemberError>;
  using ResultTargetIsNotThis = Result<const NameExpr *, TargetCheckerNotThisError>;
  using ResultIndexIsNotBytes = Result<const Type *, TargetCheckerIndexNotBytesError>;
  using ResultObjectIsDefined = Result<const Type *, TargetCheckerObjectDefinedError>;
  using ResultIndexIsWritable = Result<const Type *, TargetCheckerIndexWritableError>;
  using ResultFieldIsWritable = Result<const Type *, TargetCheckerMemberWritableError>;
  using ResultVariableMutable = Result<const VarSymbol *, TargetCheckerVariableMutableError>;

  ResultTargetIsNotThis checkNotThis(const NameExpr *name) const;
  ResultIndexIsNotBytes checkIndexNotBytes(const Type *type) const;
  ResultObjectIsDefined checkObjectDefined(const Type *type) const;
  ResultIndexIsWritable checkIndexWritable(const Type *type) const;
  ResultDefinedDictType checkDictDefined(const Type *type, const MemberExpr *member) const;
  ResultNoBytesProperty checkBytesMember(const Type *type, const MemberExpr *member) const;
  ResultVariableMutable checkVariableMutable(const NameExpr *name, const VarSymbol *variable) const;
  ResultFieldIsWritable checkMemberWritable(const Type *type, const MemberExpr *member,
                                            AssignForm form) const;
  std::string wrongTargetMessage(const Expr *target) const;
  const Type *dictTargetType(Expr *target, AssignForm form);
  const Type *indexTargetType(Expr *&target, IndexExpr *index, AssignForm form);
  const Type *typedArrayTargetType(IndexExpr *index, const Type *type);
  std::optional<const Type *> genericIndexType(IndexExpr *index);
  const Type *keyedTargetType(Expr *&target, IndexExpr *index, AssignForm form);
  const Type *arrayTargetType(IndexExpr *index);
  const Type *memberTargetType(Expr *&target, MemberExpr *member, AssignForm form);
  const Type *unionTargetType(MemberExpr *member, const Type *type, AssignForm form);
  const Type *plainMemberTargetType(MemberExpr *member, const Type *type, AssignForm form);
  void walkVariableWritable(const NameExpr *name, VarSymbol *variable);

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
