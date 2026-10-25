#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
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

FunctionChecker::FunctionChecker(Sema &sema) : sema_(sema) {}

void FunctionChecker::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = sema_.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  // A generic function: its type parameters are visible in its signature;
  // so are those of the generic class of a method (not of a static one,
  // which every instance of the class shares).
  // A parameter `xs: Iterable<int>` makes it generic too (see iterableParam).
  bool generic = !fn->typeParams.empty() || llvm::any_of(fn->params, [&](const Param &p) {
    return p.type && sema_.iterableChecker().isIterableRef(*p.type);
  });
  bool classParams = fn->cls && !fn->isStatic && !fn->cls->typeParams.empty();
  if (generic || classParams) {
    sema_.typeScopes_.emplace_back();
    if (classParams) {
      sema_.genericClassChecker().bindClassTypeParams(fn->cls);
    }
  }
  if (generic) {
    if (var) {
      // A parameter `Iterable<T>` alone is reported with it (see iterableParam).
      if (!fn->typeParams.empty()) {
        sema_.diags_.error(fn->loc(), "a generic function can be declared only at the top level for now");
      }
      fn->typeArgs.assign(fn->typeParams.size(), Type::error());
      sema_.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
    } else {
      fn->typeArgs = sym->typeParams = sema_.typeParams().declareTypeParams(fn->typeParams, &sym->typeDefaults);
    }
  }
  // A hook of node:stream in the program's own stream class (`_read()`,
  // `_transform(chunk, encoding, callback)`): the parameters the stream
  // passes, those left out too, as node allows (sema_stream.cpp).
  std::optional<std::vector<const Type *>> hook = fn->cls ? sema_.chunkTypes().streamHookParams(fn) : std::nullopt;
  if (hook && llvm::none_of(fn->params, [](const Param &p) { return p.rest; })) {
    for (size_t i = fn->params.size(); i < hook->size(); ++i) {
      Param hidden;
      hidden.name = sema_.ctx_.hiddenName();
      hidden.loc = fn->loc();
      fn->params.push_back(hidden);
    }
  }
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    // Only the parameters of the methods and constructors of classes have
    // decorators (docs/decorators.md, section 1).
    if (!param.decorators.empty() && (!fn->cls || fn->cls->literal) && !fn->isInstance) {
      sema_.diags_.error(param.decorators[0].loc, "a decorator of a parameter can decorate only a "
                                            "parameter of a method or a constructor of a class");
    }
    const Type *type = nullptr;
    if (param.type) {
      type = sema_.iterableChecker().iterableParam(fn, sym, *param.type);
      bool outerList = std::exchange(sema_.listContext_, param.rest);
      if (!type) {
        type = sema_.typeResolver().resolveType(*param.type, /*allowVoid=*/false);
      }
      sema_.listContext_ = outerList;
      // `_read(size: number)` of node: the size is an int.
      if (hook && i < hook->size() && type && type->isFloat() && (*hook)[i]->isInt()) {
        type = (*hook)[i];
      }
    } else if (hook && i < hook->size() && !param.rest) {
      type = (*hook)[i];
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
      // `function f(n = 10)`: n is an int.
    } else if (param.defaultValue && !param.rest && !fn->source.empty()) {
      // `function f(n = limit * 2)`: the type of the value, found when
      // needed (see resolveParams).
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      sema_.diags_.error(param.loc, "parameter '" + param.label() + "' needs a type annotation");
      type = Type::error();
    }
    if (param.optional) {
      type = Type::optional(type);
    }
    param.declaredType = type;
    // `function (this: unknown, ...args: A)`: the `this` the function gets,
    // which `f.call(obj, ...)` passes first (docs/decorators.md, section 2).
    if (i == 0 && param.name == "this" && !param.rest && param.type) {
      if (fn->isArrow) {
        sema_.diags_.error(param.loc, "an arrow function cannot have a 'this' parameter: it sees the "
                                "'this' of the function around it; write a 'function'");
      } else if (fn->cls) {
        sema_.diags_.error(param.loc,
                     "a method cannot have a 'this' parameter: its 'this' is its object");
      } else {
        sym->hasThis = !param.optional && !param.defaultValue;
      }
    }
    if (param.rest) {
      // The arguments from here on, as an array (empty without any); or a
      // list of the types of parameters, `...args: A`.
      if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
        sema_.diags_.error(param.loc,
                     "a rest parameter must be an array, e.g. '..." + param.name + ": int[]'");
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    // A caller may leave out an optional parameter or one with a default
    // value, or pass undefined: then the default applies.
    bool mayOmit = param.optional || param.defaultValue;
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      if (afterOptional) {
        sema_.diags_.error(param.loc, "a required parameter cannot follow an optional one");
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional;
  }
  // Without an annotation: void if no `return` gives a value, else what the
  // body returns, found when needed (see returnType). A generator returns an
  // iterator, whose types its body gives.
  if (fn->returnType) {
    sym->ret = sema_.asyncChecker().asyncReturnType(fn, sema_.generatorChecker().generatorReturnType(fn, sema_.predicateChecker().resolveReturnType(fn, sym)));
  } else if (!ReturnInferrer::returnsValue(fn->body) && fn->isAsync && !fn->isGenerator) {
    sym->ret = Type::promise(Type::voidTy());
  } else if (!ReturnInferrer::returnsValue(fn->body) && !fn->isGenerator) {
    sym->ret = Type::voidTy();
  }
  if (generic || classParams) {
    sema_.typeScopes_.pop_back();
  }
  // `...args: [int, int]`: those parameters (see expandArgumentList).
  sema_.argListChecker().expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped) {
    sema_.scopes().declare(fn->name, fn->loc(), {var, sym});
  }
}

void FunctionChecker::checkFunctionBody(FunctionDecl *fn) {
  if (fn->external) {
    sema_.tzModulesChecker().checkExternal(fn);
    return;
  }
  FuncSymbol *sym = fn->sym;
  sema_.returnInferrer().resolveParams(sym);
  fn->checked = true;
  FuncSymbol *outer = sema_.currentFunction_;
  sema_.currentFunction_ = sym;
  sema_.functions_.push_back(fn);
  unsigned errors = sema_.diags_.errorCount();
  // A method may use the private members of its class; a closure those of
  // the class around it.
  ClassDecl *outerClass = sema_.currentClass_;
  if (!fn->isClosure()) {
    sema_.currentClass_ = fn->cls;
  }
  FunctionDecl *outerInit = sema_.initFn_;
  const VarSymbol *outerThis = sema_.initThis_;
  bool outerReady = sema_.closureThisReady_;
  sema_.pushScope();
  // The type parameters of a generic function, or the types an instance has
  // for them; and those of the class of a method.
  if (fn->cls && !fn->isStatic) {
    sema_.genericClassChecker().bindClassTypeParams(fn->cls);
  }
  // A class that a class expression in a copy of a generic function made
  // sees the types of the copy.
  if (fn->cls) {
    sema_.lateClasses().bindOuterTypeParams(fn->cls);
  }
  if (!fn->typeArgs.empty()) {
    sema_.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
  }
  if (fn->takesThis()) {
    VarSymbol *self = sema_.classChecker().declareThis(fn);
    if (fn->isConstructor || fn == fn->cls->init) {
      // The fields set when it starts: the optional ones, which hold
      // undefined, and in a constructor those with initializers. In a class
      // that extends another, the initializers run after `super(...)`, and
      // the fields declared up the chain are set by then.
      sema_.initFn_ = fn;
      sema_.initThis_ = self;
      sema_.closureThisReady_ = true;
      sema_.flow_.setFields.clear();
      bool derived = fn->cls->parent != nullptr;
      for (const ClassField &f : fn->cls->fields) {
        if (!f.isStatic && (f.optional || (fn->isConstructor && !derived && f.init && !f.method) ||
                            (!fn->isConstructor && f.redeclared))) {
          sema_.flow_.setFields.push_back(f.name);
        }
      }
      sema_.flow_.beforeSuper = derived && fn->isConstructor;
      if (sema_.flow_.beforeSuper) {
        // `super(...)` must be a statement of the body itself.
        unsigned supers = 0;
        for (Stmt *stmt : fn->body->body) {
          auto *expr = dyn_cast<ExprStmt>(stmt);
          auto *call = expr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
          auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
          if (callee && callee->name == "super") {
            sema_.rootSupers_.insert(call);
            ++supers;
          }
        }
        if (supers == 0) {
          sema_.diags_.error(fn->loc(), "the constructor of '" + fn->cls->name +
                                      "' must call 'super(...)': '" + fn->cls->name +
                                      "' extends '" + fn->cls->parent->name +
                                      "', whose constructor sets the fields it declares");
        }
      }
    }
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    // A default value is computed at each call that leaves the parameter
    // out, where it sees the parameters before it.
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue && type->isError() && !param.type) {
      // Its type comes from it, and checking it failed (see resolveParams).
      param.defaultValue->type = type;
      param.sym = sema_.scopes().declareVar(param.name, param.loc, type, /*isConst=*/false);
      continue;
    }
    if (param.defaultValue) {
      llvm::StringSet<> names;
      collectNames(param.defaultValue, names);
      bool forward = false;
      for (size_t j = i; j < fn->params.size() && !forward; ++j) {
        if (names.contains(fn->params[j].name)) {
          sema_.diags_.error(param.defaultValue->loc(),
                       "the default value of '" + param.label() + "' uses '" + fn->params[j].name +
                           "', which is not set yet: a default value can "
                           "use only the parameters before it");
          forward = true;
        }
      }
      if (forward) {
        param.defaultValue->type = Type::error();
        param.sym = sema_.scopes().declareVar(param.name, param.loc, type, /*isConst=*/false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(param.defaultValue); fe && fe->fn->name.empty()) {
        fe->fn->name = param.name; // as JavaScript names it
      }
      param.defaultValue = sema_.exprChecker().checkExpr(param.defaultValue, type->isError() ? nullptr : type);
      param.defaultValue = sema_.typeRelations().coerce(param.defaultValue, type);
    }
    param.sym = sema_.scopes().declareVar(param.name, param.loc, type, /*isConst=*/false);
    if (param.defaultValue && type->isOptional() && !param.defaultValue->type->isOptional() &&
        !param.defaultValue->type->isUndefined()) {
      sema_.flowChecker().setDefined(param.sym, true);
    }
  }
  // The callback of a method of a typed array knows what its element holds.
  sema_.byteProofs().addByteCallbackFacts(fn);
  // `args` of `...args: [int, int]`, whose parameters are those.
  sema_.argListChecker().declareArgumentList(fn);
  // The parameters and the outermost block of the body share one scope. A
  // function that returns a boolean summarizes what its returns show.
  sema_.predicateChecker().beginPredicate(fn);
  sema_.closureChecker().checkStatements(fn->body);
  if (!sema_.flow_.unreachable && sym->asserts) {
    sema_.predicateChecker().notePredicateExit(sema_.flow_, Sema::Flow(), fn->body->loc());
  }
  sema_.predicateChecker().endPredicate(fn);
  if (fn->isConstructor && sema_.initFn_ == fn && !sema_.flow_.beforeSuper) {
    sema_.classChecker().checkFieldsSet(fn->loc());
  }
  sema_.popScope();
  sema_.functions_.pop_back();
  sema_.currentFunction_ = outer;
  sema_.currentClass_ = outerClass;
  sema_.initFn_ = outerInit;
  sema_.initThis_ = outerThis;
  sema_.closureThisReady_ = outerReady;

  // As in TypeScript, a function that may return undefined may also just end.
  // A generator that may finish without `return` returns undefined.
  const Type *ret = fn->isAsync && sym->ret && sym->ret->isPromise() ? sym->ret->promised()
                    : !fn->isGenerator                               ? sym->ret
                    : sym->ret && (sym->ret->isIterator() || sym->ret->isAsyncIterator())
                        ? sym->ret->returned()
                        : nullptr;
  if (ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() && !ret->isUnknown() &&
      !ReturnChecker::alwaysReturns(fn->body)) {
    sema_.diags_.error(fn->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                ret->name() + "' on every path");
  }
  if (sym->isGeneric() && sema_.diags_.errorCount() > errors) {
    sema_.brokenGenerics_.insert(sym);
  }
  if (fn->cls && fn->cls->isGeneric() && sema_.diags_.errorCount() > errors) {
    sema_.brokenClasses_.insert(fn->cls);
  }
}

} // namespace tz
