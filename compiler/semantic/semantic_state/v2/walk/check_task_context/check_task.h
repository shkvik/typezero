#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/union/union/union.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class CheckTaskKind {
  USE_MODULE,
  RESOLVE_TYPE_REF,
  RESOLVE_ALIAS,
  COMPLETE_CLASS,
  MARK_OVERRIDDEN,
  WALK_HOOK_PARAMS,
  REPORT_VARIANT_PAIR,
  START_DECLARATION_FILE,
  FINISH_DECLARATION_FILE,
  START_DECLARED_ALIAS,
  FINISH_DECLARED_ALIAS,
  START_DECLARED_FUNCTION,
  START_DECLARED_CLASS,
  FINISH_DECLARED_CLASS,
  START_DECLARED_MEMBER,
  FINISH_DECLARED_FIELD,
  START_DECLARED_PARAM,
  FINISH_DECLARED_PARAM,
  FINISH_DECLARED_RETURN,
  START_LINK_CLASSES,
  FINISH_LINK_CLASSES,
  START_LINK_OVERRIDES,
  FINISH_LINK_OVERRIDES,
  LINK_OVERRIDE,
};

struct UseModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::USE_MODULE;
  Module *module;
};

struct ResolveTypeRef {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_TYPE_REF;
  TypeRef *ref;
  bool isVoidAllowed;
};

struct ResolveAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ALIAS;
  TypeAliasDecl *alias;
};

struct CompleteClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::COMPLETE_CLASS;
  ClassDecl *classDecl;
};

struct MarkOverridden {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_OVERRIDDEN;
  FunctionDecl *functionDecl;
};

struct WalkHookParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_HOOK_PARAMS;
  const FunctionDecl *functionDecl;
  std::vector<FunctionDecl *> inherited;
};

struct ReportVariantPair {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_VARIANT_PAIR;
  FunctionDecl *first;
  FunctionDecl *second;
};

struct StartDeclarationFile {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARATION_FILE;
  Module *module;
};

struct FinishDeclarationFile {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARATION_FILE;
  Module *module;
};

struct StartDeclaredAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_ALIAS;
  TypeAliasDecl *declared;
};

struct FinishDeclaredAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_ALIAS;
  TypeAliasDecl *declared;
  TypeAliasDecl *implemented;
};

struct StartDeclaredFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_FUNCTION;
  FunctionDecl *declared;
};

struct StartDeclaredClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_CLASS;
  DeclaredClass *declared;
};

struct FinishDeclaredClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_CLASS;
  const DeclaredClass *declared;
  const ClassDecl *classDecl;
};

struct StartDeclaredMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_MEMBER;
  DeclaredMember *member;
  const ClassDecl *classDecl;
};

struct FinishDeclaredField {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_FIELD;
  const DeclaredMember *member;
  const ClassField *field;
  const ClassDecl *classDecl;
};

struct DeclaredSignatureMatch {
  std::vector<Param> *declared;
  const FunctionDecl *implemented;
  FuncSymbol *returned;
  std::optional<TypeRef> *returnType;
  llvm::SMLoc loc;
  std::string what;
  bool isConstructor;
};

struct StartDeclaredParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_PARAM;
  DeclaredSignatureMatch match;
  size_t index;
};

struct FinishDeclaredParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_PARAM;
  DeclaredSignatureMatch match;
  size_t index;
};

struct FinishDeclaredReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_RETURN;
  DeclaredSignatureMatch match;
};

struct StartLinkClasses {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LINK_CLASSES;
};

struct FinishLinkClasses {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_CLASSES;
  std::vector<ClassDecl *> order;
};

struct StartLinkOverrides {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LINK_OVERRIDES;
  ClassDecl *classDecl;
};

struct FinishLinkOverrides {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_OVERRIDES;
  const ClassDecl *classDecl;
};

struct LinkOverride {
  static constexpr CheckTaskKind kind = CheckTaskKind::LINK_OVERRIDE;
  ClassDecl *classDecl;
  FunctionDecl *functionDecl;
};

using CheckTask =
    Union<UseModule, ResolveTypeRef, ResolveAlias, CompleteClass, MarkOverridden, WalkHookParams,
          ReportVariantPair, StartDeclarationFile, FinishDeclarationFile, StartDeclaredAlias,
          FinishDeclaredAlias, StartDeclaredFunction, StartDeclaredClass, FinishDeclaredClass,
          StartDeclaredMember, FinishDeclaredField, StartDeclaredParam, FinishDeclaredParam,
          FinishDeclaredReturn, StartLinkClasses, FinishLinkClasses, StartLinkOverrides,
          FinishLinkOverrides, LinkOverride>;

}
