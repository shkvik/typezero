#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class CheckTaskKind {
  USE_MODULE,
  RESOLVE_TYPE_REF,
  RESOLVE_INNER_TYPE_REF,
  RESOLVE_OBJECT_FIELDS,
  DECLARE_CLASS_TYPE_PARAMS,
  BIND_CLASS_TYPE_PARAMS,
  DECLARE_FUNCTION_TYPE_PARAMS,
  BIND_FUNCTION_TYPE_PARAMS,
  INSTANTIATE_OWN_ALIAS,
  SUBSTITUTE_INSTANCE_FIELDS,
  INSTANTIATE_EMITTER_PARENT,
  MARK_OVERRIDDEN,
  FIND_STREAM_HOOK_PARAMS,
  WALK_HOOK_PARAMS,
  WALK_ITERABLE_PARAM,
  WALK_GENERATOR_RETURN,
  WALK_ASYNC_RETURN,
  WALK_STMT,
  REPORT_UNLISTENED_ERRORS,
  ENTER_LOAD_BODY,
  LEAVE_LOAD_BODY,
  IMPORT_BUILTIN_MODULE,
  RESOLVE_IMPORT,
  START_TOP_LEVEL,
  NEXT_TOP_LEVEL_ITEM,
  FINISH_TOP_LEVEL,
  CLOSE_TOP_LEVEL,
  START_LOAD_EARLY,
  LOAD_EARLY_BODY,
  FINISH_LOAD_EARLY,
  BIND_LOADED_NAMES,
  RESOLVE_ALIAS,
  START_ALIAS,
  ENTER_ALIAS,
  FINISH_ALIAS,
  RESOLVE_ENUM,
  ENTER_ENUM,
  NEXT_ENUM_MEMBER,
  START_FUNCTION_SYMBOL,
  FINISH_FUNCTION_SYMBOL,
  START_FUNCTION_PARAMS,
  START_FUNCTION_PARAM,
  FINISH_FUNCTION_PARAM,
  START_FUNCTION_RETURN,
  FINISH_FUNCTION_RETURN,
  START_RETURN_TYPE,
  FINISH_ASSERTS_RETURN,
  FINISH_PREDICATE_RETURN,
  RESOLVE_CLASS,
  OPEN_CLASS_SCOPE,
  CLOSE_CLASS_SCOPE,
  RESOLVE_CLASS_MEMBERS,
  START_CLASS_FIELD,
  FINISH_CLASS_FIELD,
  START_CLASS_BASES,
  FINISH_CLASS_BASES,
  FINISH_PARENT_CLASS,
  FINISH_LINK_PARENT,
  FINISH_IMPLEMENTED,
  ADD_BASE,
  DECLARE_METHODS,
  FINISH_CONSTRUCTOR,
  FINISH_CLASS_METHOD,
  START_FIELD_METHOD,
  FINISH_FIELD_METHOD,
  START_TYPED_METHOD,
  FINISH_TYPED_PARAM,
  FINISH_TYPED_METHOD,
  FINISH_METHODS,
  NEXT_FIELD_DEFAULT,
  CONTINUE_FIELD_DEFAULT,
  MAKE_FIELD_INIT,
  FINISH_FIELD_INIT,
  FINISH_VARIANTS,
  FINISH_ITEM_METHODS,
  COMPLETE_CLASS,
  FINISH_COMPLETION,
  START_INTERFACE_FIELDS,
  START_INTERFACE_BASE,
  FINISH_INTERFACE_BASE,
  FINISH_INTERFACE_FIELDS,
  REPORT_VARIANT_PAIR,
  START_LINK_CLASSES,
  FINISH_LINK_CLASSES,
  START_LINK_OVERRIDES,
  FINISH_LINK_OVERRIDES,
  LINK_OVERRIDE,
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

struct FunctionSymbolDraft {
  FunctionDecl *functionDecl;
  FuncSymbol *function;
  VarSymbol *variable;
  bool isNamed;
  bool isTypeScoped;
};

struct EnumCount {
  double next;
  bool isCounting;
  bool hasNumbers;
  bool hasStrings;
};

struct EnumFrame {
  EnumDecl *enumDecl;
  std::optional<Module *> previousModule;
  size_t nextMember;
  EnumCount count;
};

struct InterfaceDraft {
  TypeAliasDecl *alias;
  std::vector<Field> fields;
  std::vector<std::string> origins;
  std::vector<Overload> overloads;
  std::vector<const Type *> bases;
};

enum class DefaultStepKind {
  VALUE,
  ENTER_EMPTY,
  RESOLVE_FAMILY,
  LEAVE_EMPTY,
  MAKE_NEW,
  FILL_OBJECT
};

struct DefaultFor {
  static constexpr DefaultStepKind kind = DefaultStepKind::VALUE;
  const Type *type;
  const TypeRef *ref;
};

struct EnterEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::ENTER_EMPTY;
  ClassDecl *classDecl;
};

struct ResolveFamily {
  static constexpr DefaultStepKind kind = DefaultStepKind::RESOLVE_FAMILY;
  ClassDecl *classDecl;
};

struct LeaveEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::LEAVE_EMPTY;
  ClassDecl *classDecl;
};

struct MakeNew {
  static constexpr DefaultStepKind kind = DefaultStepKind::MAKE_NEW;
  ClassDecl *classDecl;
};

struct FillObject {
  static constexpr DefaultStepKind kind = DefaultStepKind::FILL_OBJECT;
  ObjectLitExpr *object;
  std::vector<const Field *> fields;
};

using DefaultStep = Union<DefaultFor, EnterEmpty, ResolveFamily, LeaveEmpty, MakeNew, FillObject>;

struct DefaultRun {
  llvm::SmallPtrSet<const ClassDecl *, 4> visiting;
  llvm::SMLoc loc;
  std::vector<DefaultStep> steps;
  std::vector<Expr *> values;
  bool hasNoDefault;
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

struct ResolveInnerTypeRef {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_INNER_TYPE_REF;
  TypeRef *ref;
};

struct ResolveObjectFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_OBJECT_FIELDS;
  TypeRef *ref;
};

struct DeclareClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_CLASS_TYPE_PARAMS;
  ClassDecl *classDecl;
};

struct BindClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_CLASS_TYPE_PARAMS;
  const ClassDecl *classDecl;
};

struct DeclareFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_FUNCTION_TYPE_PARAMS;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct BindFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_FUNCTION_TYPE_PARAMS;
  const FunctionDecl *functionDecl;
};

struct InstantiateOwnAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_OWN_ALIAS;
  TypeAliasDecl *alias;
};

struct SubstituteInstanceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_INSTANCE_FIELDS;
  ClassDecl *instance;
};

struct InstantiateEmitterParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_EMITTER_PARENT;
  ClassDecl *classDecl;
  ClassDecl *emitter;
  TypeRef *ref;
};

struct MarkOverridden {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_OVERRIDDEN;
  FunctionDecl *functionDecl;
};

struct FindStreamHookParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_STREAM_HOOK_PARAMS;
  FunctionDecl *functionDecl;
};

struct WalkHookParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_HOOK_PARAMS;
  const FunctionDecl *functionDecl;
  std::vector<FunctionDecl *> inherited;
};

struct WalkIterableParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_ITERABLE_PARAM;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
  TypeRef *ref;
};

struct WalkGeneratorReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_GENERATOR_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct WalkAsyncReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_ASYNC_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct WalkStmt {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_STMT;
  Stmt *stmt;
};

struct ReportUnlistenedErrors {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_UNLISTENED_ERRORS;
};

struct EnterLoadBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_LOAD_BODY;
};

struct LeaveLoadBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_LOAD_BODY;
};

struct ImportBuiltinModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::IMPORT_BUILTIN_MODULE;
  Module *module;
  const ImportDecl *importDecl;
};

struct ResolveImport {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_IMPORT;
  Module *module;
  const ImportDecl *importDecl;
};

struct StartTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TOP_LEVEL;
  Module *module;
  llvm::StringRef until;
};

struct NextTopLevelItem {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TOP_LEVEL_ITEM;
  Module *module;
  llvm::StringRef until;
};

struct FinishTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TOP_LEVEL;
  Module *module;
  Module *outer;
};

struct CloseTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_TOP_LEVEL;
  Module *module;
  Module *outer;
};

struct StartLoadEarly {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LOAD_EARLY;
  Module *source;
  llvm::StringRef name;
};

struct LoadEarlyBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LOAD_EARLY_BODY;
  Module *source;
  llvm::StringRef name;
};

struct FinishLoadEarly {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LOAD_EARLY;
  Module *source;
};

struct BindLoadedNames {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_LOADED_NAMES;
  Module *source;
};

struct ResolveAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ALIAS;
  TypeAliasDecl *alias;
};

struct StartAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_ALIAS;
  TypeAliasDecl *alias;
};

struct EnterAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_ALIAS;
  TypeAliasDecl *alias;
  std::optional<Module *> previousModule;
};

struct FinishAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ALIAS;
  TypeAliasDecl *alias;
  std::optional<Module *> previousModule;
  bool isBuiltinModule;
};

struct ResolveEnum {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ENUM;
  EnumDecl *enumDecl;
};

struct EnterEnum {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_ENUM;
  EnumDecl *enumDecl;
  std::optional<Module *> previousModule;
};

struct NextEnumMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_ENUM_MEMBER;
  EnumFrame frame;
};

struct StartFunctionSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_SYMBOL;
  FunctionDecl *functionDecl;
  VarSymbol *variable;
  bool isNamed;
};

struct FinishFunctionSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_SYMBOL;
  FunctionSymbolDraft draft;
};

struct StartFunctionParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_PARAMS;
  FunctionSymbolDraft draft;
};

struct StartFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_PARAM;
  FunctionSymbolDraft draft;
  size_t index;
};

struct FinishFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_PARAM;
  FunctionSymbolDraft draft;
  size_t index;
  bool isListContext;
};

struct StartFunctionReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_RETURN;
  FunctionSymbolDraft draft;
};

struct FinishFunctionReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_RETURN;
  FunctionSymbolDraft draft;
};

struct StartReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_RETURN_TYPE;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct FinishAssertsReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ASSERTS_RETURN;
  const FunctionDecl *functionDecl;
};

struct FinishPredicateReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PREDICATE_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct ResolveClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_CLASS;
  ClassDecl *classDecl;
};

struct OpenClassScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::OPEN_CLASS_SCOPE;
  ClassDecl *classDecl;
};

struct CloseClassScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_CLASS_SCOPE;
};

struct ResolveClassMembers {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_CLASS_MEMBERS;
  ClassDecl *classDecl;
};

struct StartClassField {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLASS_FIELD;
  ClassDecl *classDecl;
  size_t index;
};

struct FinishClassField {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_FIELD;
  ClassDecl *classDecl;
  size_t index;
  TypeRef *written;
};

struct StartClassBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLASS_BASES;
  ClassDecl *classDecl;
};

struct FinishClassBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_BASES;
  ClassDecl *classDecl;
};

struct FinishParentClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PARENT_CLASS;
  ClassDecl *classDecl;
  Stmt *found;
};

struct FinishLinkParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_PARENT;
  ClassDecl *classDecl;
  ClassDecl *parent;
};

struct FinishImplemented {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_IMPLEMENTED;
  ClassDecl *classDecl;
  TypeRef *ref;
};

struct AddBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_BASE;
  const Type *type;
  const Type *base;
};

struct DeclareMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_METHODS;
  ClassDecl *classDecl;
};

struct FinishConstructor {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONSTRUCTOR;
  FunctionDecl *constructor;
};

struct FinishClassMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_METHOD;
  const ClassDecl *classDecl;
  FunctionDecl *method;
};

struct StartFieldMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FIELD_METHOD;
  ClassDecl *classDecl;
  size_t index;
};

struct FinishFieldMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FIELD_METHOD;
  ClassDecl *classDecl;
  size_t index;
};

struct StartTypedMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TYPED_METHOD;
  FunctionDecl *functionDecl;
  const Type *declared;
};

struct FinishTypedParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPED_PARAM;
  FunctionDecl *functionDecl;
  const Type *declared;
  size_t index;
};

struct FinishTypedMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPED_METHOD;
  FunctionDecl *functionDecl;
  const Type *declared;
};

struct FinishMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_METHODS;
  ClassDecl *classDecl;
};

struct NextFieldDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_FIELD_DEFAULT;
  ClassDecl *classDecl;
  size_t fieldIndex;
};

struct ContinueFieldDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONTINUE_FIELD_DEFAULT;
  ClassDecl *classDecl;
  size_t fieldIndex;
  DefaultRun run;
};

struct MakeFieldInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::MAKE_FIELD_INIT;
  ClassDecl *classDecl;
};

struct FinishFieldInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FIELD_INIT;
  ClassDecl *classDecl;
  FunctionDecl *init;
};

struct FinishVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VARIANTS;
  ClassDecl *classDecl;
};

struct FinishItemMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITEM_METHODS;
  ClassDecl *classDecl;
  Module *module;
};

struct CompleteClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::COMPLETE_CLASS;
  ClassDecl *classDecl;
};

struct FinishCompletion {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_COMPLETION;
  ClassDecl *classDecl;
};

struct StartInterfaceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INTERFACE_FIELDS;
  TypeAliasDecl *alias;
};

struct StartInterfaceBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INTERFACE_BASE;
  InterfaceDraft draft;
  size_t index;
};

struct FinishInterfaceBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTERFACE_BASE;
  InterfaceDraft draft;
  size_t index;
};

struct FinishInterfaceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTERFACE_FIELDS;
  InterfaceDraft draft;
};

struct ReportVariantPair {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_VARIANT_PAIR;
  FunctionDecl *first;
  FunctionDecl *second;
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

using CheckTask = Union<
    UseModule, ResolveTypeRef, ResolveInnerTypeRef, ResolveObjectFields, DeclareClassTypeParams,
    BindClassTypeParams, DeclareFunctionTypeParams, BindFunctionTypeParams, InstantiateOwnAlias,
    SubstituteInstanceFields, InstantiateEmitterParent, MarkOverridden, FindStreamHookParams,
    WalkHookParams, WalkIterableParam, WalkGeneratorReturn, WalkAsyncReturn, WalkStmt,
    ReportUnlistenedErrors, EnterLoadBody, LeaveLoadBody, ImportBuiltinModule, ResolveImport,
    StartTopLevel, NextTopLevelItem, FinishTopLevel, CloseTopLevel, StartLoadEarly, LoadEarlyBody,
    FinishLoadEarly, BindLoadedNames, ResolveAlias, StartAlias, EnterAlias, FinishAlias,
    ResolveEnum, EnterEnum, NextEnumMember, StartFunctionSymbol, FinishFunctionSymbol,
    StartFunctionParams, StartFunctionParam, FinishFunctionParam, StartFunctionReturn,
    FinishFunctionReturn, StartReturnType, FinishAssertsReturn, FinishPredicateReturn, ResolveClass,
    OpenClassScope, CloseClassScope, ResolveClassMembers, StartClassField, FinishClassField,
    StartClassBases, FinishClassBases, FinishParentClass, FinishLinkParent, FinishImplemented,
    AddBase, DeclareMethods, FinishConstructor, FinishClassMethod, StartFieldMethod,
    FinishFieldMethod, StartTypedMethod, FinishTypedParam, FinishTypedMethod, FinishMethods,
    NextFieldDefault, ContinueFieldDefault, MakeFieldInit, FinishFieldInit, FinishVariants,
    FinishItemMethods, CompleteClass, FinishCompletion, StartInterfaceFields, StartInterfaceBase,
    FinishInterfaceBase, FinishInterfaceFields, ReportVariantPair, StartLinkClasses,
    FinishLinkClasses, StartLinkOverrides, FinishLinkOverrides, LinkOverride, StartDeclarationFile,
    FinishDeclarationFile, StartDeclaredAlias, FinishDeclaredAlias, StartDeclaredFunction,
    StartDeclaredClass, FinishDeclaredClass, StartDeclaredMember, FinishDeclaredField,
    StartDeclaredParam, FinishDeclaredParam, FinishDeclaredReturn>;

}
