#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
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
  if (!name->typeArgs.empty() && (entry.var() || entry.func())) {
    this->checker.diags_.error(
        name->loc(), "'" + name->name + "' is not a generic function; remove the type arguments");
  }
  if (entry.var()) {
    if (this->checker.staticInit_ && this->checker.currentClass_ &&
        this->checker.currentClass_->expression() && !entry.var()->isGlobal() &&
        llvm::is_contained(this->checker.currentClass_->locals, name->name)) {
      this->checker.diags_.error(name->loc(),
                                 "a class expression cannot use '" + name->name +
                                     "', a variable of the function around it: the class is made "
                                     "once, not at each call, and its static fields get their "
                                     "values the first time; pass the value to its constructor, "
                                     "or keep it in a global");
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
    this->checker.diags_.error(name->loc(), "'" + name->name +
                                                "' can be used as a value only where a function "
                                                "type is expected, e.g. 'xs.map(" +
                                                name->name + ")'");
  } else if (result == "Result") {
    this->checker.diags_.error(name->loc(), "'" + name->name +
                                                "' can only be used through its functions, e.g. '" +
                                                name->name + ".combine(results)'");
  } else if (result == "Ok" || result == "Err") {
    this->checker.diags_.error(
        name->loc(), "'" + name->name + "' can be used only as a type; make a Result with '" +
                         (result == "Ok" ? "ok(value)" : "err(error)") + "', and test one with '" +
                         (result == "Ok" ? "isOk()" : "isErr()") + "'");
  } else if (name->name == "this") {
    this->checker.diags_.error(
        name->loc(), "'this' exists only in the methods of classes, which are not supported yet");
  } else if (llvm::is_contained(builtins, name->name) || llvm::is_contained(uris, name->name)) {
    this->checker.diags_.error(name->loc(),
                               "'" + name->name +
                                   "' can be used as a value only where a function type "
                                   "is expected, e.g. 'const f: (s: string) => " +
                                   (llvm::is_contained(uris, name->name) ? "string" : "double") +
                                   " = " + name->name + "'");
  } else if (name->name == "super") {
    this->checker.diags_.error(name->loc(),
                               "'super' is not a value: call the constructor of the class "
                               "extended, 'super(...)', or a method of it, 'super.m(...)'");
  } else if (name->name == "arguments") {
    this->checker.diags_.error(
        name->loc(), "'arguments' is not supported; use a rest parameter, e.g. '...args: int[]'");
  } else if (name->name == "BigInt") {
    this->checker.decimalChecker().reportBigInt(name->loc(), "'BigInt'");
  } else if (name->name == "decimal") {
    this->checker.diags_.error(name->loc(),
                               "'decimal' is used as 'decimal(x)', which makes a decimal of x, or "
                               "through its functions, e.g. 'decimal.max(a, b)'");
  } else if (name->name == "null") {
    this->checker.diags_.error(
        name->loc(), "'null' is not supported: the only value for nothing is 'undefined'; "
                     "write 'undefined' (null exists only in Json values)");
  } else if (const char *why = whyUnsupported(name->name)) {
    this->checker.diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
  } else if (entry.enumDecl()) {
    this->checker.diags_.error(name->loc(), "enum '" + name->name + "' can only be used as '" +
                                                name->name + ".Member' or '" + name->name +
                                                "[value]'");
  } else if (entry.cls()) {
    return this->checker.classValueChecker().checkClassValue(name, entry.cls());
  } else if (this->checker.currentClass_ && this->checker.currentClass_->expression() &&
             !this->checker.currentClass_->name.empty() &&
             this->checker.currentClass_->name == name->name) {
    return this->checker.classValueChecker().checkClassValue(name, this->checker.currentClass_);
  } else if (this->checker.currentClass_ && this->checker.currentClass_->expression() &&
             llvm::is_contained(this->checker.currentClass_->locals, name->name)) {
    this->checker.diags_.error(name->loc(),
                               "a class expression cannot use '" + name->name +
                                   "', a variable of the function around it: the class is made "
                                   "once, not at each call, and its code runs later; pass the "
                                   "value to its constructor, or keep it in a global");
  } else if (TopLevelChecker::ResultSourceIsLoaded loaded =
                 this->checker.topLevelChecker().checkLoaded(name);
             loaded.isErr()) {
    if (loaded.error().code != TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED) {
      this->checker.diags_.error(name->loc(), loaded.error().message);
    }
  } else if (this->checker.early_ &&
             (this->checker.state(this->checker.module_).topLevelNames.contains(name->name) ||
              this->checker.declaredLater(name->name))) {
    this->checker.diags_.error(
        name->loc(), "'" + name->name + "' is declared after the code that first calls '" +
                         this->checker.early_->name + "', which needs to know what '" +
                         this->checker.early_->name +
                         "' returns; move the declaration up, or declare the return type");
  } else if (const Checker::BlockNames *later = this->checker.declaredLater(name->name);
             later && this->checker.functions_.size() > later->functions) {
    this->checker.diags_.error(name->loc(),
                               "'" + name->name +
                                   "' is declared later in the block, after this function "
                                   "is created; move the declaration up, or declare the "
                                   "function with 'function', which can use it");
  } else if (later) {
    this->checker.diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
  } else if (llvm::is_contained(this->checker.initializing_, name->name)) {
    this->checker.diags_.error(
        name->loc(), "'" + name->name + "' is used in its own initializer, before it has a value");
  } else if (object != std::end(objects)) {
    this->checker.diags_.error(
        name->loc(), "'" + name->name + "' can only be used through its properties, e.g. '" +
                         object->second + "'");
  } else {
    this->checker.diags_.error(name->loc(), "cannot find name '" + name->name + "'");
  }
  name->type = Type::error();

  return name;
}

}
