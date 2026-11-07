#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/expr/call_checker/call_checker_errors.h"
#include "compiler/check/expr/new_checker/new_checker_errors.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

NameChecker::NameChecker(Checker &owner)
    : checker(owner) {}

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
  const ClassDecl *current = this->checker.currentClass_;
  bool captured = this->checker.staticInit_ && current && current->expression() &&
                  !entry.var()->isGlobal() && llvm::is_contained(current->locals, name->name);
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
      entry.enumDecl() ? llvm::StringRef() : this->checker.resultTypes().resultName(name->name);
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
    this->checker.decimalChecker().reportBigInt(name->loc(), "'BigInt'");

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
    Expr *value = this->checker.classValueChecker().checkClassValue(name, entry.cls());

    return ok(value);
  }
  ClassDecl *current = this->checker.currentClass_;
  bool expression = current && current->expression();
  if (expression && !current->name.empty() && current->name == name->name) {
    Expr *value = this->checker.classValueChecker().checkClassValue(name, current);

    return ok(value);
  }
  if (expression && llvm::is_contained(current->locals, name->name)) {
    std::string message = ErrorTextNameUse::laterUse(name->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }
  TopLevelChecker::ResultSourceIsLoaded loaded = this->checker.topLevelChecker().checkLoaded(name);
  if (loaded.isErr()) {
    bool reported = loaded.error().code == TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED;

    return err(NameCheckerUnknownNameError{
        .code = reported ? NameCheckerUnknownNameErrorCode::ALREADY_REPORTED
                         : NameCheckerUnknownNameErrorCode::NOT_LOADED,
        .message = loaded.error().message,
    });
  }
  bool earlyCall = this->checker.early_ &&
                   (this->checker.state(this->checker.module_).topLevelNames.contains(name->name) ||
                    this->checker.declaredLater(name->name));
  if (earlyCall) {
    std::string message = ErrorTextNameUse::afterCall(name->name, this->checker.early_->name);

    return err(NameCheckerUnknownNameError{
        .code = NameCheckerUnknownNameErrorCode::DECLARED_AFTER_CALL,
        .message = message,
    });
  }
  const Checker::BlockNames *later = this->checker.declaredLater(name->name);
  if (later && this->checker.functions_.size() > later->functions) {
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
  if (llvm::is_contained(this->checker.initializing_, name->name)) {
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
    ClassChecker::ResultThisReference reference = this->checker.classChecker().checkThis(name);
    if (reference.isErr()) {
      this->checker.diags_.error(name->loc(), reference.error().message);
      return name;
    }
    return reference.value();
  }
  if (name->cls() && name->type) {
    return name;
  }
  if (Expr *node = this->checker.builtinModuleChecker().checkNodeName(name)) {
    return node;
  }
  ScopeEntry entry = this->checker.topLevelChecker().lookupValue(name->name);
  ResultNoTypeArguments plain = NameChecker::typeArgsAbsent(name, entry);
  if (plain.isErr()) {
    this->checker.diags_.error(name->loc(), plain.error().message);
  }
  if (entry.var()) {
    ResultVariableVisible visibleHere = this->notCaptured(name, entry);
    if (visibleHere.isErr()) {
      this->checker.diags_.error(name->loc(), visibleHere.error().message);
    }
    name->binding = NamedVar{.var = entry.var()};
    entry.var()->read = true;
    this->checker.nestedFunctions().noteUse(entry.var(), name->loc());
    name->type = this->checker.scopes().varType(entry.var());
    Scopes::ResultNameIsVisible visible = this->checker.scopes().checkVisible(name, entry.var());
    if (visible.isErr()) {
      this->checker.diags_.error(name->loc(), visible.error().message);
    }
    if (!this->checker.streamViews_.empty()) {
      Checker::FieldPath path;
      path.var = entry.var();
      if (const Type *view = this->checker.setEncodingChecker().streamViewOf(path, name->loc())) {
        return this->checker.typeRelations().convert(name, view);
      }
    }
    if (const Type *narrowed = this->checker.genericChecker().narrowedType(entry.var())) {
      if (narrowed->isOptional() && this->checker.isDefined(entry.var())) {
        narrowed = narrowed->inner();
      }
      return this->checker.typeRelations().convert(name, narrowed);
    }
    bool defined = name->type->isOptional() && this->checker.isDefined(entry.var());
    const Type *type = defined ? name->type->inner() : name->type;
    if (type->isResult()) {
      if (this->checker.flow_.okResults.contains(entry.var()) && !type->errType()->isNever()) {
        return this->checker.typeRelations().convert(name,
                                                     Type::result(type->okType(), Type::never()));
      }
      if (this->checker.flow_.errResults.contains(entry.var()) && !type->okType()->isNever()) {
        return this->checker.typeRelations().convert(name,
                                                     Type::result(Type::never(), type->errType()));
      }
    }
    if (defined) {
      return this->checker.typeRelations().convert(name, type);
    }
    return name;
  }
  if (!entry.func() && !entry.enumDecl()) {
    if (Expr *constant =
            this->checker.constantFolder().globalConstant("", name->name, name->loc())) {
      return constant;
    }
  }
  if (entry.func()) {
    name->binding = NamedFunction{.func = entry.func()};
    name->type = this->checker.returnInferrer().functionType(entry.func());
    this->checker.tzModulesChecker().noteHttpsValue(entry.func()->decl, name->loc());
    return name;
  }
  if (!entry.found()) {
    if (ClassDecl *cls = this->checker.metadataChecker().builtinConstructor(name->name)) {
      return this->checker.classValueChecker().checkClassValue(name, cls);
    }
  }
  ResultResolvedNameUse resolved = this->unknownName(name, entry);
  if (resolved.isOk()) {
    return resolved.value();
  }
  if (resolved.error().code != NameCheckerUnknownNameErrorCode::ALREADY_REPORTED) {
    this->checker.diags_.error(name->loc(), resolved.error().message);
  }
  name->type = Type::error();

  return name;
}

}
