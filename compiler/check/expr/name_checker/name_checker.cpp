#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

NameChecker::NameChecker(Sema &sema) : sema_(sema) {}

Expr *NameChecker::checkName(NameExpr *name) {
  if (name->name == "this") {
    return sema_.classChecker().checkThis(name);
  }
  // A class as a value that Sema made (see classValueAt), which may have no
  // name.
  if (name->cls && name->type) {
    return name;
  }
  if (Expr *node = sema_.builtinModuleChecker().checkNodeName(name)) {
    return node;
  }
  Sema::ScopeEntry entry = sema_.topLevelChecker().lookupValue(name->name);
  if (!name->typeArgs.empty() && (entry.var || entry.func)) {
    sema_.diags_.error(name->loc(),
                 "'" + name->name + "' is not a generic function; remove the type arguments");
  }
  if (entry.var) {
    // A static field of a class expression gets its value once, when the
    // class is made, not at each call of the function around it.
    if (sema_.staticInit_ && sema_.currentClass_ && sema_.currentClass_->expression && !entry.var->isGlobal &&
        llvm::is_contained(sema_.currentClass_->locals, name->name)) {
      sema_.diags_.error(name->loc(), "a class expression cannot use '" + name->name +
                                    "', a variable of the function around it: the class is made "
                                    "once, not at each call, and its static fields get their "
                                    "values the first time; pass the value to its constructor, "
                                    "or keep it in a global");
    }
    name->var = entry.var;
    sema_.nestedFunctions().noteUse(entry.var, name->loc());
    name->type = sema_.scopes().varType(entry.var);
    sema_.scopes().checkVisible(name, entry.var);
    // A stream after its setEncoding() (sema_stream.cpp).
    if (!sema_.streamViews_.empty()) {
      Sema::FieldPath path;
      path.var = entry.var;
      if (const Type *view = sema_.setEncodingChecker().streamViewOf(path, name->loc())) {
        return sema_.typeRelations().convert(name, view);
      }
    }
    // Of a type parameter, with its type known here from `typeof`.
    if (const Type *narrowed = sema_.genericChecker().narrowedType(entry.var)) {
      return sema_.typeRelations().convert(name, narrowed);
    }
    // Narrowed: known to hold a value here.
    if (name->type->isOptional() && sema_.isDefined(entry.var)) {
      return sema_.typeRelations().convert(name, name->type->inner());
    }
    // A Result known to be an Ok is a `Result<T, never>`, whose value can be
    // read; one known to be an Err, a `Result<never, E>`.
    if (const Type *type = name->type; type->isResult()) {
      if (sema_.flow_.okResults.contains(entry.var) && !type->errType()->isNever()) {
        return sema_.typeRelations().convert(name, Type::result(type->okType(), Type::never()));
      }
      if (sema_.flow_.errResults.contains(entry.var) && !type->okType()->isNever()) {
        return sema_.typeRelations().convert(name, Type::result(Type::never(), type->errType()));
      }
    }
    return name;
  }
  if (!entry.func && !entry.enumDecl) {
    if (Expr *constant = sema_.constantFolder().globalConstant("", name->name, name->loc())) {
      return constant;
    }
  }
  if (entry.func) {
    // A top-level function as a value.
    name->func = entry.func;
    name->type = sema_.returnInferrer().functionType(entry.func);
    sema_.tzModulesChecker().noteHttpsValue(entry.func->decl, name->loc());
    return name;
  }
  // `String`, `Date`... as values: the classes that records `design:*` hold
  // (docs/decorators.md, section 3).
  if (!entry.var && !entry.func && !entry.enumDecl && !entry.cls) {
    if (ClassDecl *cls = sema_.metadataChecker().builtinConstructor(name->name)) {
      return sema_.classValueChecker().checkClassValue(name, cls);
    }
  }
  static const llvm::StringLiteral builtins[] = {"String", "Number",   "parseInt", "parseFloat",
                                                 "isNaN",  "isFinite", "Boolean"};
  static const llvm::StringLiteral uris[] = {"encodeURIComponent", "encodeURI",
                                             "decodeURIComponent", "decodeURI"};
  // The global objects, by what is used of them first.
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> objects[] = {
      {"process", "process.argv"},
      {"console", "console.log(x)"},
      {"Date", "Date.now()"},
      {"performance", "performance.now()"},
      {"RegExp", "new RegExp(pattern)"},
      {"JSON", "JSON.parse(text)"}};
  auto object = llvm::find_if(objects, [&](const auto &o) { return o.first == name->name; });
  llvm::StringRef neverthrow = entry.enumDecl ? llvm::StringRef() : sema_.resultTypes().neverthrowName(name->name);
  if (neverthrow == "ok" || neverthrow == "err") {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' can be used as a value only where a function "
                                  "type is expected, e.g. 'xs.map(" +
                                  name->name + ")'");
  } else if (neverthrow == "Result") {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' can only be used through its functions, e.g. '" + name->name +
                                  ".combine(results)'");
  } else if (neverthrow == "Ok" || neverthrow == "Err") {
    sema_.diags_.error(name->loc(),
                 "'" + name->name + "' can be used only as a type; make a Result with '" +
                     (neverthrow == "Ok" ? "ok(value)" : "err(error)") + "', and test one with '" +
                     (neverthrow == "Ok" ? "isOk()" : "isErr()") + "'");
  } else if (neverthrow == "fromThrowable") {
    sema_.diags_.error(name->loc(), "'" + name->name + "' can only be called: '" + name->name + "(f)'");
  } else if (neverthrow == "safeTry") {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' is not supported yet; call it: 'safeTry(function* () { ... "
                                  "})'");
  } else if (neverthrow == "ResultAsync") {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' can only be used through its functions, e.g. '" + name->name +
                                  ".fromPromise(p, toError)'");
  } else if (!neverthrow.empty()) {
    sema_.diags_.error(name->loc(), "'" + name->name + "' can only be called: '" + name->name + "(...)'");
  } else if (name->name == "this") {
    sema_.diags_.error(name->loc(),
                 "'this' exists only in the methods of classes, which are not supported yet");
  } else if (llvm::is_contained(builtins, name->name) || llvm::is_contained(uris, name->name)) {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' can be used as a value only where a function type "
                                  "is expected, e.g. 'const f: (s: string) => " +
                                  (llvm::is_contained(uris, name->name) ? "string" : "double") +
                                  " = " + name->name + "'");
  } else if (name->name == "super") {
    sema_.diags_.error(name->loc(), "'super' is not a value: call the constructor of the class "
                              "extended, 'super(...)', or a method of it, 'super.m(...)'");
  } else if (name->name == "arguments") {
    sema_.diags_.error(name->loc(),
                 "'arguments' is not supported; use a rest parameter, e.g. '...args: int[]'");
  } else if (name->name == "BigInt") {
    sema_.decimalChecker().reportBigInt(name->loc(), "'BigInt'");
  } else if (name->name == "decimal") {
    sema_.diags_.error(name->loc(), "'decimal' is used as 'decimal(x)', which makes a decimal of x, or "
                              "through its functions, e.g. 'decimal.max(a, b)'");
  } else if (name->name == "null") {
    sema_.diags_.error(name->loc(), "'null' is not supported: the only value for nothing is 'undefined'; "
                              "write 'undefined' (null exists only in Json values)");
  } else if (const char *why = whyUnsupported(name->name)) {
    sema_.diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
  } else if (entry.enumDecl) {
    sema_.diags_.error(name->loc(), "enum '" + name->name + "' can only be used as '" + name->name +
                                  ".Member' or '" + name->name + "[value]'");
  } else if (entry.cls) {
    return sema_.classValueChecker().checkClassValue(name, entry.cls);
  }
  // A class expression sees its own name in its body, as in JavaScript.
  else if (sema_.currentClass_ && sema_.currentClass_->expression && !sema_.currentClass_->name.empty() &&
           sema_.currentClass_->name == name->name) {
    return sema_.classValueChecker().checkClassValue(name, sema_.currentClass_);
  } else if (sema_.currentClass_ && sema_.currentClass_->expression &&
             llvm::is_contained(sema_.currentClass_->locals, name->name)) {
    sema_.diags_.error(name->loc(), "a class expression cannot use '" + name->name +
                                  "', a variable of the function around it: the class is made "
                                  "once, not at each call, and its code runs later; pass the "
                                  "value to its constructor, or keep it in a global");
  } else if (sema_.topLevelChecker().reportNotLoaded(name)) {
    // A name of a file that is checked later: reported.
  } else if (sema_.early_ &&
             (sema_.state(sema_.module_).topLevelNames.contains(name->name) || sema_.declaredLater(name->name))) {
    sema_.diags_.error(name->loc(), "'" + name->name + "' is declared after the code that first calls '" +
                                  sema_.early_->name + "', which needs to know what '" + sema_.early_->name +
                                  "' returns; move the declaration up, or declare the return type");
  } else if (const Sema::BlockNames *later = sema_.declaredLater(name->name);
             later && sema_.functions_.size() > later->functions) {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' is declared later in the block, after this function "
                                  "is created; move the declaration up, or declare the "
                                  "function with 'function', which can use it");
  } else if (later) {
    sema_.diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
  } else if (llvm::is_contained(sema_.initializing_, name->name)) {
    sema_.diags_.error(name->loc(),
                 "'" + name->name + "' is used in its own initializer, before it has a value");
  } else if (object != std::end(objects)) {
    sema_.diags_.error(name->loc(), "'" + name->name +
                                  "' can only be used through its properties, e.g. '" +
                                  object->second + "'");
  } else {
    sema_.diags_.error(name->loc(), "cannot find name '" + name->name + "'");
  }
  name->type = Type::error();

  return name;
}

} // namespace tz
