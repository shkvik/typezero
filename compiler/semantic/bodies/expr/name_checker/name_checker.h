#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ResultTypes;
class ClassChecker;
class TypeRelations;
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

  enum class UnknownNameKind {
    OK_AS_VALUE,
    RESULT_VALUE,
    TYPE_ONLY,
    THIS_OUTSIDE,
    BUILTIN_VALUE,
    SUPER_VALUE,
    ARGUMENTS,
    BIG_INT_VALUE,
    DECIMAL_VALUE,
    NULL_VALUE,
    UNSUPPORTED,
    ENUM_VALUE,
    CLASS_VALUE,
    OWN_CLASS_EXPRESSION,
    CAPTURED_LOCAL,
    NOT_LOADED,
    DECLARED_AFTER_CALL,
    DECLARED_LATER_IN_BLOCK,
    USED_BEFORE_DECLARED,
    OWN_INITIALIZER,
    OBJECT_VALUE,
    UNKNOWN_NAME,
  };

  ResultResolvedNameUse unknownName(NameExpr *name, const ScopeEntry &entry);
  ResultVariableVisible notCaptured(const NameExpr *name, const ScopeEntry &entry) const;
  static ResultNoTypeArguments typeArgsAbsent(const NameExpr *name, const ScopeEntry &entry);

  static bool isUriName(std::optional<GlobalName> global);
  static bool isConversionName(std::optional<GlobalName> global);
  static std::optional<llvm::StringRef> objectExample(std::optional<GlobalName> global);
  static std::string builtinValueText(llvm::StringRef text, std::optional<GlobalName> global);
  static ResultResolvedNameUse unknownError(NameCheckerUnknownNameErrorCode code,
                                            const std::string &message);
  Expr *checkThisName(NameExpr *name);
  bool isDeclaredAfterCall(llvm::StringRef text) const;
  std::string typeOnlyText(const NameExpr *name) const;
  std::string afterCallText(const NameExpr *name) const;
  Expr *narrowedName(NameExpr *name, VarSymbol *var);
  Expr *checkFunctionName(NameExpr *name, FuncSymbol *func);
  ResultResolvedNameUse notLoadedError(const NameExpr *name);
  ResultResolvedNameUse reportBigIntName(const NameExpr *name);
  Expr *checkVariableName(NameExpr *name, const ScopeEntry &entry);
  Expr *reportUnknownName(NameExpr *name, const ScopeEntry &entry);
  UnknownNameKind unknownNameKind(const NameExpr *name, const ScopeEntry &entry) const;
  UnknownNameKind laterNameKind(const NameExpr *name, std::optional<GlobalName> global) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  ResultTypes *resultTypes = nullptr;
  ClassChecker *classChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
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
