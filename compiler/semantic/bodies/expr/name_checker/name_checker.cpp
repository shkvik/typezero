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
  static const GlobalName builtins[] = {GlobalName::STRING, GlobalName::NUMBER,
                                        GlobalName::PARSE_INT, GlobalName::PARSE_FLOAT,
                                        GlobalName::BOOLEAN};
  static const GlobalName uris[] = {GlobalName::ENCODE_URI_COMPONENT, GlobalName::ENCODE_URI,
                                    GlobalName::DECODE_URI_COMPONENT, GlobalName::DECODE_URI};
  static const std::pair<GlobalName, llvm::StringLiteral> objects[] = {
      {GlobalName::PROCESS, "process.argv"},
      {GlobalName::CONSOLE, "console.log(x)"},
      {GlobalName::DATE, "Date.now()"},
      {GlobalName::PERFORMANCE, "performance.now()"},
      {GlobalName::REG_EXP, "new RegExp(pattern)"},
      {GlobalName::JSON, "JSON.parse(text)"}};
  std::optional<GlobalName> global = GlobalNameTable::find(name->name);
  auto object = llvm::find_if(objects, [&](const auto &o) { return o.first == global; });
  std::optional<GlobalName> result = std::nullopt;
  if (!entry.enumDecl()) {
    result = this->resultTypes->resultName(name->name);
  }
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    std::string message = ErrorTextNameUse::okValue(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::OK_AS_VALUE,
        .message = message,
    });
  }
  if (result == GlobalName::RESULT) {
    std::string message = ErrorTextNameUse::resultUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::RESULT_VALUE,
        .message = message,
    });
  }
  if (result == GlobalName::OK_TYPE || result == GlobalName::ERR_TYPE) {
    llvm::StringRef make = result == GlobalName::OK_TYPE ? "ok(value)" : "err(error)";
    llvm::StringRef test = result == GlobalName::OK_TYPE ? "isOk()" : "isErr()";
    std::string message = ErrorTextNameUse::typeOnly(name->name, make, test);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::TYPE_ONLY,
        .message = message,
    });
  }
  if (name->name == "this") {
    std::string message = ErrorTextNameUse::noThis();

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::THIS_OUTSIDE,
        .message = message,
    });
  }
  bool uri = global.has_value() && llvm::is_contained(uris, *global);
  bool builtin = global.has_value() && llvm::is_contained(builtins, *global);
  if (builtin || uri) {
    llvm::StringRef returns = uri ? "string" : "double";
    std::string message = ErrorTextNameUse::builtin(name->name, returns);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::BUILTIN_VALUE,
        .message = message,
    });
  }
  if (name->name == "super") {
    std::string message = ErrorTextNameUse::superUse();

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::SUPER_VALUE,
        .message = message,
    });
  }
  if (name->name == "arguments") {
    std::string message = ErrorTextNameUse::arguments();

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::ARGUMENTS,
        .message = message,
    });
  }
  if (global == GlobalName::BIG_INT) {
    std::string quoted = ErrorTextBigInt::quoted(name->name);
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(name->loc(), bigint);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::DECIMAL) {
    std::string message = ErrorTextNameUse::decimal();

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::DECIMAL_VALUE,
        .message = message,
    });
  }
  if (name->name == "null") {
    std::string message = ErrorTextNameUse::nullUse();

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::NULL_VALUE,
        .message = message,
    });
  }
  std::string why = ErrorTextUnsupported::why(name->name);
  if (!why.empty()) {
    std::string message = ErrorTextCallName::noSupport(name->name, why);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::UNSUPPORTED,
        .message = message,
    });
  }
  if (entry.enumDecl()) {
    std::string message = ErrorTextNameUse::enumUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::ENUM_VALUE,
        .message = message,
    });
  }
  if (entry.cls()) {
    Expr *value = this->classValueChecker->checkClassValue(name, entry.cls());

    return ok(value);
  }
  ClassDecl *current = this->state.getClassContext().getCurrentClass();
  bool expression = current && current->expression();
  if (expression && !current->name.empty() && current->name == name->name) {
    Expr *value = this->classValueChecker->checkClassValue(name, current);

    return ok(value);
  }
  if (expression && llvm::is_contained(current->locals, name->name)) {
    std::string message = ErrorTextNameUse::laterUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }
  TopLevelChecker::ResultSourceIsLoaded loaded = this->topLevelChecker->checkLoaded(name);
  if (loaded.isErr()) {
    bool reported = loaded.error().code == TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED;

    return err(NameCheckerUnknownNameError{
        .code = reported ? NameCheckerUnknownNameErrorCode::ALREADY_REPORTED
                         : NameCheckerUnknownNameErrorCode::NOT_LOADED,
        .message = loaded.error().message,
    });
  }
  bool earlyCall = this->state.getFunctionStack().getEarlyFunction() &&
                   (this->state.getModuleStates()
                        .getModuleState(this->state.getModuleStates().getCurrentModule())
                        .topLevelNames.contains(name->name) ||
                    this->state.getScopeStack().findDeclaredLater(name->name));
  if (earlyCall) {
    std::string message = ErrorTextNameUse::afterCall(
        name->name, this->state.getFunctionStack().getEarlyFunction()->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::DECLARED_AFTER_CALL,
        .message = message,
    });
  }
  const BlockNames *later = this->state.getScopeStack().findDeclaredLater(name->name);
  if (later && this->state.getFunctionStack().getFunctionDepth() > later->functions) {
    std::string message = ErrorTextNameUse::laterBlock(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::DECLARED_LATER_IN_BLOCK,
        .message = message,
    });
  }
  if (later) {
    std::string message = ErrorTextNameUse::beforeDecl(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }
  if (this->state.getExpressionMarks().isInitializing(name->name)) {
    std::string message = ErrorTextNameUse::ownInit(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::OWN_INITIALIZER,
        .message = message,
    });
  }
  if (object != std::end(objects)) {
    std::string message = ErrorTextNameUse::objectUse(name->name, object->second);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::OBJECT_VALUE,
        .message = message,
    });
  }
  std::string message = ErrorTextNameUse::notFound(name->name);

  return err(NameCheckerUnknownNameError{
      .code = NameCheckerUnknownNameErrorCode::UNKNOWN_NAME,
      .message = message,
  });
}

Expr *NameChecker::checkName(NameExpr *name) {
  if (name->name == "this") {
    ClassChecker::ResultThisReference reference = this->classChecker->checkThis(name);
    if (reference.isErr()) {
      this->state.getDiagnostics().error(name->loc(), reference.error().message);
      return name;
    }
    return reference.value();
  }
  if (name->cls() && name->type) {
    return name;
  }
  if (Expr *node = this->builtinModuleChecker->checkNodeName(name)) {
    return node;
  }
  ScopeEntry entry = this->topLevelChecker->lookupValue(name->name);
  ResultNoTypeArguments plain = NameChecker::typeArgsAbsent(name, entry);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(name->loc(), plain.error().message);
  }
  if (entry.var()) {
    ResultVariableVisible visibleHere = this->notCaptured(name, entry);
    if (visibleHere.isErr()) {
      this->state.getDiagnostics().error(name->loc(), visibleHere.error().message);
    }
    name->binding = NamedVar{.var = entry.var()};
    entry.var()->read = true;
    this->nestedFunctions->noteUse(entry.var(), name->loc());
    name->type = this->scopes->varType(entry.var());
    Scopes::ResultNameIsVisible visible = this->scopes->checkVisible(name, entry.var());
    if (visible.isErr()) {
      this->state.getDiagnostics().error(name->loc(), visible.error().message);
    }
    if (this->state.getStreamStore().hasStreamViews()) {
      FieldPath path;
      path.var = entry.var();
      if (const Type *view = this->state.getStreamStore().findStreamView(path, name->loc())) {
        return this->typeRelations->convert(name, view);
      }
    }
    if (const Type *narrowed = this->genericChecker->narrowedType(entry.var())) {
      if (narrowed->isOptional() && this->state.getFlowState().isDefined(entry.var())) {
        narrowed = narrowed->inner();
      }
      return this->typeRelations->convert(name, narrowed);
    }
    bool defined = name->type->isOptional() && this->state.getFlowState().isDefined(entry.var());
    const Type *type = defined ? name->type->inner() : name->type;
    if (type->isResult()) {
      if (this->state.getFlowState().isOkResult(entry.var()) && !type->errType()->isNever()) {
        return this->typeRelations->convert(name, Type::result(type->okType(), Type::never()));
      }
      if (this->state.getFlowState().isErrResult(entry.var()) && !type->okType()->isNever()) {
        return this->typeRelations->convert(name, Type::result(Type::never(), type->errType()));
      }
    }
    if (defined) {
      return this->typeRelations->convert(name, type);
    }
    return name;
  }
  if (entry.func()) {
    name->binding = NamedFunction{.func = entry.func()};
    name->type = this->returnInferrer->functionType(entry.func());
    this->tzModulesChecker->noteHttpsValue(entry.func()->decl, name->loc());
    return name;
  }
  if (!entry.found()) {
    if (ClassDecl *cls = this->metadataChecker->builtinConstructor(name->name)) {
      return this->classValueChecker->checkClassValue(name, cls);
    }
  }
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
