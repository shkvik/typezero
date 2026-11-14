#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/names/code_names_registry_errors.h"
#include "compiler/semantic/declaration/modules/declaration_modules_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class CodeNamesRegistry {
public:
  explicit CodeNamesRegistry(SemanticState &owner);

  void checkName(const CheckName &task);
  void lookupName(const LookupName &task);
  void variableTypeKnown(const VariableTypeKnown &task);
  void finishFunctionName(const FinishFunctionName &task);
  void finishVariableName(const FinishVariableName &task);

private:
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

  using ResultResolvedNameUse = Result<Expr *, CodeNamesRegistryUnknownNameError>;
  using ResultVariableVisible = Result<const NameExpr *, CodeNamesRegistryNotCapturedError>;
  using ResultNoTypeArguments = Result<const NameExpr *, CodeNamesRegistryTypeArgsAbsentError>;
  using ResultSourceIsLoaded = Result<const NameExpr *, DeclarationModulesRegistryCheckLoadedError>;
  using ResultVisibleVariable =
      Result<const VarSymbol *, DeclarationNamesRegistryCheckVisibleError>;

  void startFunctionName(NameExpr *name, FuncSymbol *function);
  void reportUnknownName(NameExpr *name, const ScopeEntry &entry);
  void startVariableName(NameExpr *name, const ScopeEntry &entry);


  bool isDeclaredAfterCall(llvm::StringRef text) const;
  bool isUriName(std::optional<GlobalName> global) const;
  bool isConversionName(std::optional<GlobalName> global) const;


  std::string typeOnlyText(const NameExpr *name) const;
  std::string afterCallText(const NameExpr *name) const;
  std::string builtinValueText(llvm::StringRef text, std::optional<GlobalName> global) const;
  std::string loadLabel(llvm::StringRef name, llvm::StringRef declared, const Module *source) const;


  const Type *streamView(VarSymbol *variable, llvm::SMLoc loc) const;
  const Type *shownType(const NameExpr *name, const VarSymbol *variable) const;
  const Type *definedType(const Type *narrowed, const VarSymbol *variable) const;
  const Type *knownResultType(const Type *type, const VarSymbol *variable) const;


  UnknownNameKind unknownNameKind(const NameExpr *name, const ScopeEntry &entry) const;
  UnknownNameKind laterNameKind(const NameExpr *name, std::optional<GlobalName> global) const;


  ResultResolvedNameUse checkNotLoaded(const NameExpr *name);
  ResultResolvedNameUse reportBigIntName(const NameExpr *name);
  ResultResolvedNameUse checkUnknownName(NameExpr *name, const ScopeEntry &entry);
  ResultResolvedNameUse unknownError(CodeNamesRegistryUnknownNameErrorCode code,
                                     const std::string &message) const;


  ClassDecl *builtinClass(const NameExpr *name, const ScopeEntry &entry) const;
  ResultSourceIsLoaded checkLoaded(const NameExpr *name) const;
  ResultVisibleVariable checkVisible(const NameExpr *name, const VarSymbol *variable) const;
  ResultVariableVisible checkNotCaptured(const NameExpr *name, const ScopeEntry &entry) const;
  std::optional<GlobalName> resultNameOf(llvm::StringRef text, const ScopeEntry &entry) const;
  ResultNoTypeArguments checkTypeArgsAbsent(const NameExpr *name, const ScopeEntry &entry) const;

  SemanticState &state;
};

}
