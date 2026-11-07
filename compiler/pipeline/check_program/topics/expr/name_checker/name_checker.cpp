#include "compiler/pipeline/check_program/topics/expr/name_checker/name_checker.h"
#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker_errors.h"
#include "compiler/pipeline/check_program/topics/expr/new_checker/new_checker_errors.h"
#include "compiler/pipeline/check_program/topics/functions/nested_functions/nested_functions.h"
#include "compiler/pipeline/check_program/topics/reflect/metadata_checker/metadata_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_types/result_types.h"
#include "compiler/pipeline/check_program/topics/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

NameChecker::NameChecker(CheckState &owner)
    : state(owner) {}

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
  static const llvm::StringLiteral builtins[] = {"String", "Number", "parseInt", "parseFloat",
                                                 "Boolean"};
  static const llvm::StringLiteral uris[] = {"encodeURIComponent", "encodeURI",
                                             "decodeURIComponent", "decodeURI"};
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> objects[] = {
      {"process", "process.argv"},
      {"console", "console.log(x)"},
      {"Date", "Date.now()"},
      {"performance", "performance.now()"},
      {"RegExp", "new RegExp(pattern)"},
      {"JSON", "JSON.parse(text)"}};
  auto object = llvm::find_if(objects, [&](const auto &o) { return o.first == name->name; });
  llvm::StringRef result =
      entry.enumDecl() ? llvm::StringRef() : ResultTypes(this->state).resultName(name->name);
  if (result == "ok" || result == "err") {
    std::string message = ErrorTextNameUse::okValue(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::OK_AS_VALUE,
        .message = message,
    });
  }
  if (result == "Result") {
    std::string message = ErrorTextNameUse::resultUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::RESULT_VALUE,
        .message = message,
    });
  }
  if (result == "Ok" || result == "Err") {
    llvm::StringRef make = result == "Ok" ? "ok(value)" : "err(error)";
    llvm::StringRef test = result == "Ok" ? "isOk()" : "isErr()";
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
  bool uri = llvm::is_contained(uris, name->name);
  if (llvm::is_contained(builtins, name->name) || uri) {
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
  if (name->name == "BigInt") {
    DecimalChecker(this->state).reportBigInt(name->loc(), "'BigInt'");

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (name->name == "decimal") {
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
    Expr *value = ClassValueChecker(this->state).checkClassValue(name, entry.cls());

    return ok(value);
  }
  ClassDecl *current = this->state.getClassContext().getCurrentClass();
  bool expression = current && current->expression();
  if (expression && !current->name.empty() && current->name == name->name) {
    Expr *value = ClassValueChecker(this->state).checkClassValue(name, current);

    return ok(value);
  }
  if (expression && llvm::is_contained(current->locals, name->name)) {
    std::string message = ErrorTextNameUse::laterUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }
  TopLevelChecker::ResultSourceIsLoaded loaded = TopLevelChecker(this->state).checkLoaded(name);
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
    ClassChecker::ResultThisReference reference = ClassChecker(this->state).checkThis(name);
    if (reference.isErr()) {
      this->state.getDiagnostics().error(name->loc(), reference.error().message);
      return name;
    }
    return reference.value();
  }
  if (name->cls() && name->type) {
    return name;
  }
  if (Expr *node = BuiltinModuleChecker(this->state).checkNodeName(name)) {
    return node;
  }
  ScopeEntry entry = TopLevelChecker(this->state).lookupValue(name->name);
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
    NestedFunctions(this->state).noteUse(entry.var(), name->loc());
    name->type = Scopes(this->state).varType(entry.var());
    Scopes::ResultNameIsVisible visible = Scopes(this->state).checkVisible(name, entry.var());
    if (visible.isErr()) {
      this->state.getDiagnostics().error(name->loc(), visible.error().message);
    }
    if (this->state.getStreamStore().hasStreamViews()) {
      FieldPath path;
      path.var = entry.var();
      if (const Type *view = this->state.getStreamStore().findStreamView(path, name->loc())) {
        return TypeRelations(this->state).convert(name, view);
      }
    }
    if (const Type *narrowed = GenericChecker(this->state).narrowedType(entry.var())) {
      if (narrowed->isOptional() && this->state.getFlowState().isDefined(entry.var())) {
        narrowed = narrowed->inner();
      }
      return TypeRelations(this->state).convert(name, narrowed);
    }
    bool defined = name->type->isOptional() && this->state.getFlowState().isDefined(entry.var());
    const Type *type = defined ? name->type->inner() : name->type;
    if (type->isResult()) {
      if (this->state.getFlowState().isOkResult(entry.var()) && !type->errType()->isNever()) {
        return TypeRelations(this->state)
            .convert(name, Type::result(type->okType(), Type::never()));
      }
      if (this->state.getFlowState().isErrResult(entry.var()) && !type->okType()->isNever()) {
        return TypeRelations(this->state)
            .convert(name, Type::result(Type::never(), type->errType()));
      }
    }
    if (defined) {
      return TypeRelations(this->state).convert(name, type);
    }
    return name;
  }
  if (!entry.func() && !entry.enumDecl()) {
    if (Expr *constant = ConstantFolder(this->state).globalConstant("", name->name, name->loc())) {
      return constant;
    }
  }
  if (entry.func()) {
    name->binding = NamedFunction{.func = entry.func()};
    name->type = ReturnInferrer(this->state).functionType(entry.func());
    TzModulesChecker(this->state).noteHttpsValue(entry.func()->decl, name->loc());
    return name;
  }
  if (!entry.found()) {
    if (ClassDecl *cls = MetadataChecker(this->state).builtinConstructor(name->name)) {
      return ClassValueChecker(this->state).checkClassValue(name, cls);
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
