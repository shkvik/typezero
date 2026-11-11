#include "compiler/semantic/bodies/expr/name_checker/name_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker_errors.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker_errors.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

NameChecker::NameChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->resultTypes = &all.getResultTypes();
  this->classChecker = &all.getClassChecker();
  this->typeRelations = &all.getTypeRelations();
  this->genericChecker = &all.getGenericChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->metadataChecker = &all.getMetadataChecker();
  this->nestedFunctions = &all.getNestedFunctions();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->classValueChecker = &all.getClassValueChecker();
  this->builtinModuleChecker = &all.getBuiltinModuleChecker();
}

NameChecker::ResultNoTypeArguments NameChecker::typeArgsAbsent(const NameExpr *name,
                                                               const ScopeEntry &entry) {
  if (!name->typeArgs.empty() && (entry.var() || entry.func())) {
    std::string message = ErrorTextCallTypeArgs::notGeneric(name->name);

    return err(NameCheckerTypeArgsAbsentError{
        .code = NameCheckerTypeArgsAbsentErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(name);
}

NameChecker::ResultVariableVisible NameChecker::notCaptured(const NameExpr *name,
                                                            const ScopeEntry &entry) const {
  const ClassDecl *current = this->state.getClassContext().getCurrentClass();
  bool captured = this->state.getClassContext().isInsideStaticInit() && current &&
                  current->expression() && !entry.var()->isGlobal() &&
                  llvm::is_contained(current->locals, name->name);
  if (captured) {
    std::string message = ErrorTextNameUse::staticUse(name->name);

    return err(NameCheckerNotCapturedError{
        .code = NameCheckerNotCapturedErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }

  return ok(name);
}

NameChecker::ResultResolvedNameUse NameChecker::unknownName(NameExpr *name,
                                                            const ScopeEntry &entry) {
  UnknownNameKind kind = this->unknownNameKind(name, entry);
  std::optional<GlobalName> global = GlobalNameTable::find(name->name);
  llvm::StringRef text = name->name;
  switch (kind) {
    case UnknownNameKind::OK_AS_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::OK_AS_VALUE,
                                       ErrorTextNameUse::okValue(text));
    case UnknownNameKind::RESULT_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::RESULT_VALUE,
                                       ErrorTextNameUse::resultUse(text));
    case UnknownNameKind::TYPE_ONLY:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::TYPE_ONLY,
                                       this->typeOnlyText(name));
    case UnknownNameKind::THIS_OUTSIDE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::THIS_OUTSIDE,
                                       ErrorTextNameUse::noThis());
    case UnknownNameKind::BUILTIN_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::BUILTIN_VALUE,
                                       NameChecker::builtinValueText(text, global));
    case UnknownNameKind::SUPER_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::SUPER_VALUE,
                                       ErrorTextNameUse::superUse());
    case UnknownNameKind::ARGUMENTS:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::ARGUMENTS,
                                       ErrorTextNameUse::arguments());
    case UnknownNameKind::BIG_INT_VALUE:
      return this->reportBigIntName(name);
    case UnknownNameKind::DECIMAL_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::DECIMAL_VALUE,
                                       ErrorTextNameUse::decimal());
    case UnknownNameKind::NULL_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::NULL_VALUE,
                                       ErrorTextNameUse::nullUse());
    case UnknownNameKind::UNSUPPORTED:
      return NameChecker::unknownError(
          NameCheckerUnknownNameErrorCode::UNSUPPORTED,
          ErrorTextCallName::noSupport(text, ErrorTextUnsupported::why(text)));
    case UnknownNameKind::ENUM_VALUE:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::ENUM_VALUE,
                                       ErrorTextNameUse::enumUse(text));
    case UnknownNameKind::CLASS_VALUE:
      return ok(this->classValueChecker->checkClassValue(name, entry.cls()));
    case UnknownNameKind::OWN_CLASS_EXPRESSION:
      return ok(this->classValueChecker->checkClassValue(
          name, this->state.getClassContext().getCurrentClass()));
    case UnknownNameKind::CAPTURED_LOCAL:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::CAPTURED_LOCAL,
                                       ErrorTextNameUse::laterUse(text));
    case UnknownNameKind::NOT_LOADED:
      return this->notLoadedError(name);
    case UnknownNameKind::DECLARED_AFTER_CALL:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::DECLARED_AFTER_CALL,
                                       this->afterCallText(name));
    case UnknownNameKind::DECLARED_LATER_IN_BLOCK:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::DECLARED_LATER_IN_BLOCK,
                                       ErrorTextNameUse::laterBlock(text));
    case UnknownNameKind::USED_BEFORE_DECLARED:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::USED_BEFORE_DECLARED,
                                       ErrorTextNameUse::beforeDecl(text));
    case UnknownNameKind::OWN_INITIALIZER:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::OWN_INITIALIZER,
                                       ErrorTextNameUse::ownInit(text));
    case UnknownNameKind::OBJECT_VALUE:
      return NameChecker::unknownError(
          NameCheckerUnknownNameErrorCode::OBJECT_VALUE,
          ErrorTextNameUse::objectUse(text, *NameChecker::objectExample(global)));
    case UnknownNameKind::UNKNOWN_NAME:
      return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::UNKNOWN_NAME,
                                       ErrorTextNameUse::notFound(text));
  }

  return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::UNKNOWN_NAME,
                                   ErrorTextNameUse::notFound(text));
}

NameChecker::UnknownNameKind NameChecker::unknownNameKind(const NameExpr *name,
                                                          const ScopeEntry &entry) const {
  llvm::StringRef text = name->name;
  std::optional<GlobalName> global = GlobalNameTable::find(text);
  std::optional<GlobalName> result = std::nullopt;
  if (entry.enumDecl() == nullptr) {
    result = this->resultTypes->resultName(text);
  }
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    return UnknownNameKind::OK_AS_VALUE;
  }
  if (result == GlobalName::RESULT) {
    return UnknownNameKind::RESULT_VALUE;
  }
  if (result == GlobalName::OK_TYPE || result == GlobalName::ERR_TYPE) {
    return UnknownNameKind::TYPE_ONLY;
  }
  if (text == "this") {
    return UnknownNameKind::THIS_OUTSIDE;
  }
  if (NameChecker::isConversionName(global) || NameChecker::isUriName(global)) {
    return UnknownNameKind::BUILTIN_VALUE;
  }
  if (text == "super") {
    return UnknownNameKind::SUPER_VALUE;
  }
  if (text == "arguments") {
    return UnknownNameKind::ARGUMENTS;
  }
  if (global == GlobalName::BIG_INT) {
    return UnknownNameKind::BIG_INT_VALUE;
  }
  if (global == GlobalName::DECIMAL) {
    return UnknownNameKind::DECIMAL_VALUE;
  }
  if (text == "null") {
    return UnknownNameKind::NULL_VALUE;
  }
  if (!ErrorTextUnsupported::why(text).empty()) {
    return UnknownNameKind::UNSUPPORTED;
  }
  if (entry.enumDecl() != nullptr) {
    return UnknownNameKind::ENUM_VALUE;
  }
  if (entry.cls() != nullptr) {
    return UnknownNameKind::CLASS_VALUE;
  }
  return this->laterNameKind(name, global);
}

NameChecker::UnknownNameKind NameChecker::laterNameKind(const NameExpr *name,
                                                        std::optional<GlobalName> global) const {
  llvm::StringRef text = name->name;
  const ClassDecl *current = this->state.getClassContext().getCurrentClass();
  bool expression = current != nullptr && current->expression();
  if (expression && !current->name.empty() && current->name == text) {
    return UnknownNameKind::OWN_CLASS_EXPRESSION;
  }
  if (expression && llvm::is_contained(current->locals, text)) {
    return UnknownNameKind::CAPTURED_LOCAL;
  }
  if (this->topLevelChecker->checkLoaded(name).isErr()) {
    return UnknownNameKind::NOT_LOADED;
  }
  if (this->isDeclaredAfterCall(text)) {
    return UnknownNameKind::DECLARED_AFTER_CALL;
  }
  const BlockNames *later = this->state.getScopeStack().findDeclaredLater(text);
  if (later != nullptr && this->state.getFunctionStack().getFunctionDepth() > later->functions) {
    return UnknownNameKind::DECLARED_LATER_IN_BLOCK;
  }
  if (later != nullptr) {
    return UnknownNameKind::USED_BEFORE_DECLARED;
  }
  if (this->state.getExpressionMarks().isInitializing(text)) {
    return UnknownNameKind::OWN_INITIALIZER;
  }
  if (NameChecker::objectExample(global).has_value()) {
    return UnknownNameKind::OBJECT_VALUE;
  }

  return UnknownNameKind::UNKNOWN_NAME;
}

bool NameChecker::isDeclaredAfterCall(llvm::StringRef text) const {
  if (this->state.getFunctionStack().getEarlyFunction() == nullptr) {
    return false;
  }
  ModuleStates &modules = this->state.getModuleStates();
  bool topLevel = modules.getModuleState(modules.getCurrentModule()).topLevelNames.contains(text);

  return topLevel || this->state.getScopeStack().findDeclaredLater(text) != nullptr;
}

bool NameChecker::isConversionName(std::optional<GlobalName> global) {
  static const GlobalName conversions[] = {GlobalName::STRING, GlobalName::NUMBER,
                                           GlobalName::PARSE_INT, GlobalName::PARSE_FLOAT,
                                           GlobalName::BOOLEAN};

  return global.has_value() && llvm::is_contained(conversions, *global);
}

bool NameChecker::isUriName(std::optional<GlobalName> global) {
  static const GlobalName uris[] = {GlobalName::ENCODE_URI_COMPONENT, GlobalName::ENCODE_URI,
                                    GlobalName::DECODE_URI_COMPONENT, GlobalName::DECODE_URI};

  return global.has_value() && llvm::is_contained(uris, *global);
}

std::optional<llvm::StringRef> NameChecker::objectExample(std::optional<GlobalName> global) {
  static const std::pair<GlobalName, llvm::StringLiteral> objects[] = {
      {GlobalName::PROCESS, "process.argv"},
      {GlobalName::CONSOLE, "console.log(x)"},
      {GlobalName::DATE, "Date.now()"},
      {GlobalName::PERFORMANCE, "performance.now()"},
      {GlobalName::REG_EXP, "new RegExp(pattern)"},
      {GlobalName::JSON, "JSON.parse(text)"}};
  for (const std::pair<GlobalName, llvm::StringLiteral> &object : objects) {
    if (object.first == global) {
      return object.second;
    }
  }

  return std::nullopt;
}

NameChecker::ResultResolvedNameUse NameChecker::unknownError(NameCheckerUnknownNameErrorCode code,
                                                             const std::string &message) {
  return err(NameCheckerUnknownNameError{
      .code = code,
      .message = message,
  });
}

std::string NameChecker::typeOnlyText(const NameExpr *name) const {
  std::optional<GlobalName> result = this->resultTypes->resultName(name->name);
  bool okType = result == GlobalName::OK_TYPE;
  llvm::StringRef make = okType ? "ok(value)" : "err(error)";
  llvm::StringRef test = okType ? "isOk()" : "isErr()";

  return ErrorTextNameUse::typeOnly(name->name, make, test);
}

std::string NameChecker::builtinValueText(llvm::StringRef text, std::optional<GlobalName> global) {
  llvm::StringRef returns = NameChecker::isUriName(global) ? "string" : "double";

  return ErrorTextNameUse::builtin(text, returns);
}

NameChecker::ResultResolvedNameUse NameChecker::reportBigIntName(const NameExpr *name) {
  std::string quoted = ErrorTextBigInt::quoted(name->name);
  std::string bigint = ErrorTextBigInt::none(quoted);
  this->state.getDiagnostics().error(name->loc(), bigint);

  return NameChecker::unknownError(NameCheckerUnknownNameErrorCode::ALREADY_REPORTED, "");
}

NameChecker::ResultResolvedNameUse NameChecker::notLoadedError(const NameExpr *name) {
  TopLevelChecker::ResultSourceIsLoaded loaded = this->topLevelChecker->checkLoaded(name);
  bool reported = loaded.error().code == TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED;
  NameCheckerUnknownNameErrorCode code = reported
                                             ? NameCheckerUnknownNameErrorCode::ALREADY_REPORTED
                                             : NameCheckerUnknownNameErrorCode::NOT_LOADED;

  return NameChecker::unknownError(code, loaded.error().message);
}

std::string NameChecker::afterCallText(const NameExpr *name) const {
  const FuncSymbol *early = this->state.getFunctionStack().getEarlyFunction();

  return ErrorTextNameUse::afterCall(name->name, early->name);
}

Expr *NameChecker::checkName(NameExpr *name) {
  if (name->name == "this") {
    return this->checkThisName(name);
  }
  if (name->cls() != nullptr && name->type != nullptr) {
    return name;
  }
  Expr *node = this->builtinModuleChecker->checkNodeName(name);
  if (node != nullptr) {
    return node;
  }
  ScopeEntry entry = this->topLevelChecker->lookupValue(name->name);
  ResultNoTypeArguments plain = NameChecker::typeArgsAbsent(name, entry);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(name->loc(), plain.error().message);
  }
  if (entry.var() != nullptr) {
    return this->checkVariableName(name, entry);
  }
  if (entry.func() != nullptr) {
    return this->checkFunctionName(name, entry.func());
  }
  if (!entry.found()) {
    ClassDecl *cls = this->metadataChecker->builtinConstructor(name->name);
    if (cls != nullptr) {
      return this->classValueChecker->checkClassValue(name, cls);
    }
  }

  return this->reportUnknownName(name, entry);
}

Expr *NameChecker::checkThisName(NameExpr *name) {
  ClassChecker::ResultThisReference reference = this->classChecker->checkThis(name);
  if (reference.isErr()) {
    this->state.getDiagnostics().error(name->loc(), reference.error().message);
    return name;
  }

  return reference.value();
}

Expr *NameChecker::checkVariableName(NameExpr *name, const ScopeEntry &entry) {
  VarSymbol *var = entry.var();
  ResultVariableVisible visibleHere = this->notCaptured(name, entry);
  if (visibleHere.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visibleHere.error().message);
  }
  name->binding = NamedVar{.var = var};
  var->read = true;
  this->nestedFunctions->noteUse(var, name->loc());
  name->type = this->scopes->varType(var);
  Scopes::ResultNameIsVisible visible = this->scopes->checkVisible(name, var);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visible.error().message);
  }

  return this->narrowedName(name, var);
}

Expr *NameChecker::narrowedName(NameExpr *name, VarSymbol *var) {
  if (this->state.getStreamStore().hasStreamViews()) {
    FieldPath path;
    path.var = var;
    const Type *view = this->state.getStreamStore().findStreamView(path, name->loc());
    if (view != nullptr) {
      return this->typeRelations->convert(name, view);
    }
  }
  const Type *narrowed = this->genericChecker->narrowedType(var);
  if (narrowed != nullptr) {
    bool defined = narrowed->isOptional() && this->state.getFlowState().isDefined(var);
    return this->typeRelations->convert(name, defined ? narrowed->inner() : narrowed);
  }
  bool defined = name->type->isOptional() && this->state.getFlowState().isDefined(var);
  const Type *type = defined ? name->type->inner() : name->type;
  if (type->isResult()) {
    bool okKnown = this->state.getFlowState().isOkResult(var) && !type->errType()->isNever();
    if (okKnown) {
      return this->typeRelations->convert(name, Type::result(type->okType(), Type::never()));
    }
    bool errKnown = this->state.getFlowState().isErrResult(var) && !type->okType()->isNever();
    if (errKnown) {
      return this->typeRelations->convert(name, Type::result(Type::never(), type->errType()));
    }
  }
  if (defined) {
    return this->typeRelations->convert(name, type);
  }

  return name;
}

Expr *NameChecker::checkFunctionName(NameExpr *name, FuncSymbol *func) {
  name->binding = NamedFunction{.func = func};
  name->type = this->returnInferrer->functionType(func);
  this->tzModulesChecker->noteHttpsValue(func->decl, name->loc());

  return name;
}

Expr *NameChecker::reportUnknownName(NameExpr *name, const ScopeEntry &entry) {
  ResultResolvedNameUse resolved = this->unknownName(name, entry);
  if (resolved.isOk()) {
    return resolved.value();
  }
  if (resolved.error().code != NameCheckerUnknownNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(name->loc(), resolved.error().message);
  }
  name->type = Type::error();

  return name;
}
}
