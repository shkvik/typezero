#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ResultTypes;
class ClassChecker;
class TypeRelations;
class ConstantFolder;
class GenericChecker;
class ReturnInferrer;
class MetadataChecker;
class NestedFunctions;
class TopLevelChecker;
class TzModulesChecker;
class ClassValueChecker;
class BuiltinModuleChecker;

class NameChecker {
public:
  NameChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkName(NameExpr *name);

private:
  using ResultResolvedNameUse = Result<Expr *, NameCheckerUnknownNameError>;
  using ResultVariableVisible = Result<const NameExpr *, NameCheckerNotCapturedError>;
  using ResultNoTypeArguments = Result<const NameExpr *, NameCheckerTypeArgsAbsentError>;

  ResultResolvedNameUse unknownName(NameExpr *name, const ScopeEntry &entry);
  ResultVariableVisible notCaptured(const NameExpr *name, const ScopeEntry &entry) const;
  static ResultNoTypeArguments typeArgsAbsent(const NameExpr *name, const ScopeEntry &entry);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ResultTypes *resultTypes = nullptr;
  ClassChecker *classChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  MetadataChecker *metadataChecker = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
