#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/result/safe_try_checker/safe_try_checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ReturnInferrer::ReturnInferrer(Checker &checker) : checker_(checker) {}

bool ReturnInferrer::returnsValue(const Stmt *stmt) {
  if (!stmt) {
    return false;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Return: {
    // `return void f()` gives undefined, as a plain `return` does.
    const Expr *value = cast<ReturnStmt>(stmt)->value;
    auto *unary = llvm::dyn_cast_or_null<UnaryExpr>(value);
    return value && !(unary && unary->op == UnaryOp::Void);
  }
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, returnsValue);
  case Stmt::Kind::If:
    return returnsValue(cast<IfStmt>(stmt)->thenStmt) || returnsValue(cast<IfStmt>(stmt)->elseStmt);
  case Stmt::Kind::While:
    return returnsValue(cast<WhileStmt>(stmt)->body);
  case Stmt::Kind::DoWhile:
    return returnsValue(cast<DoWhileStmt>(stmt)->body);
  case Stmt::Kind::For:
    return returnsValue(cast<ForStmt>(stmt)->body);
  case Stmt::Kind::ForOf:
    return returnsValue(cast<ForOfStmt>(stmt)->body);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                        [](const SwitchCase &c) { return llvm::any_of(c.body, returnsValue); });
  case Stmt::Kind::Labeled:
    return returnsValue(cast<LabeledStmt>(stmt)->body);
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    return returnsValue(s->block) || returnsValue(s->handler) || returnsValue(s->finalizer);
  }
  default:
    return false;
  }
}

const Type *ReturnInferrer::returnType(FuncSymbol *fn) {
  if (fn->ret) {
    return fn->ret;
  }
  // Each copy of a template finds its own (see checkTemplateInstance).
  if (checker_.lateClasses().isTemplate(fn)) {
    return Type::unknown();
  }
  if (fn->inferring && checker_.suspended_.contains(fn)) {
    // Code of another file, checked out of turn because this function
    // needs it (see loadEarly), needs this function in turn.
    checker_.diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) +
                                       " returns: it uses a file that imports it back and calls it "
                                       "while loading; declare its return type, e.g. '): int'");
    checker_.suspended_.erase(fn); // reported once
    return Type::error();
  }
  if (fn->inferring) { // a call in a cycle being inferred: the type so far
    return fn->assumed ? fn->assumed : Type::error();
  }
  inferReturnType(fn);

  return fn->ret;
}

const Type *ReturnInferrer::functionType(FuncSymbol *fn) {
  resolveParams(fn);
  const Type *ret = returnType(fn);

  return Type::function(fn->params, fn->minArgs, fn->hasRest, ret ? ret : Type::error(),
                        fn->hasThis);
}

Checker::BodyState ReturnInferrer::enterBody(const FunctionDecl *fn) {
  Checker::BodyState saved;
  saved.flow = std::move(checker_.flow_);
  saved.scopes = checker_.scopes_;
  saved.typeScopes = checker_.typeScopes_;
  saved.functions = checker_.functions_;
  saved.groups = checker_.groups_;
  saved.blockNames = checker_.blockNames_;
  saved.function = checker_.currentFunction_;
  saved.targets = std::move(checker_.targets_);
  saved.pendingLabels = std::move(checker_.pendingLabels_);
  saved.otherCaseVars = checker_.otherCaseVars_;
  saved.chainShortCircuits = checker_.chainShortCircuits_;
  saved.returns = checker_.returns_;
  saved.yields = checker_.yields_;
  saved.trial = checker_.trial_;
  saved.early = checker_.early_;
  saved.module = checker_.module_;
  saved.instance = checker_.checkingInstance_;
  saved.capture = checker_.diags_.capture();
  saved.scopeBranches = checker_.scopeBranches_;
  saved.branchDepth = checker_.branchDepth_;
  checker_.flow_ = Checker::Flow();
  if (NestedGroup *group = fn ? fn->group : nullptr) {
    // A function declared in a block sees what is visible in the block,
    // wherever the check started.
    checker_.scopes_.resize(group->scopeDepth);
    checker_.typeScopes_.resize(group->scopeDepth);
    checker_.functions_.resize(group->functionDepth);
    while (!checker_.groups_.empty() && checker_.groups_.back() != group) {
      checker_.groups_.pop_back();
    }
    while (!checker_.blockNames_.empty() &&
           checker_.blockNames_.back().functions > group->functionDepth) {
      checker_.blockNames_.pop_back();
    }
  } else if (!fn || !fn->isExpression) {
    // A top-level function sees only the globals of its file.
    checker_.scopes_.resize(1);
    checker_.typeScopes_.resize(1);
    checker_.functions_.clear();
    checker_.groups_.clear();
    checker_.blockNames_.clear();
    checker_.otherCaseVars_.clear();
    checker_.trial_ = false;
    if (Module *home = fn ? checker_.declModules_.lookup(fn) : nullptr) {
      checker_.moduleDeclarer().useModule(home);
      // The variables its imports name that are declared by now: those of
      // a file whose code is being checked, in an import cycle.
      checker_.importBinder().bindImports();
    }
    // Its errors are its own, not those of an instance being checked; but
    // those of the code of a class that a copy of a template made are the
    // copy's (see checkLateClasses).
    checker_.checkingInstance_ = nullptr;
    checker_.diags_.setCapture(nullptr);
    if (Checker::Instance *owner = fn && fn->cls ? checker_.lateOwners_.lookup(fn->cls) : nullptr) {
      checker_.checkingInstance_ = owner;
      checker_.diags_.setCapture(&checker_.lateErrors_[owner]);
    }
  }
  // The branches around the scopes it sees: a function's body is not one.
  checker_.scopeBranches_.resize(checker_.scopes_.size(), 0);
  checker_.branchDepth_ = checker_.scopeBranches_.empty() ? 0 : checker_.scopeBranches_.back();
  // An arrow is checked where it is written.
  checker_.currentFunction_ = nullptr;
  checker_.targets_.clear();
  checker_.pendingLabels_.clear();
  checker_.chainShortCircuits_ = false;
  checker_.returns_ = nullptr;
  checker_.yields_ = nullptr;

  return saved;
}

void ReturnInferrer::leaveBody(Checker::BodyState &saved) {
  checker_.flow_ = std::move(saved.flow);
  checker_.scopeBranches_ = std::move(saved.scopeBranches);
  checker_.branchDepth_ = saved.branchDepth;
  checker_.scopes_ = std::move(saved.scopes);
  checker_.typeScopes_ = std::move(saved.typeScopes);
  checker_.functions_ = std::move(saved.functions);
  checker_.groups_ = std::move(saved.groups);
  checker_.blockNames_ = std::move(saved.blockNames);
  checker_.currentFunction_ = saved.function;
  checker_.targets_ = std::move(saved.targets);
  checker_.pendingLabels_ = std::move(saved.pendingLabels);
  checker_.otherCaseVars_ = std::move(saved.otherCaseVars);
  checker_.chainShortCircuits_ = saved.chainShortCircuits;
  checker_.returns_ = saved.returns;
  checker_.yields_ = saved.yields;
  checker_.trial_ = saved.trial;
  checker_.early_ = saved.early;
  checker_.module_ = saved.module;
  checker_.checkingInstance_ = saved.instance;
  checker_.diags_.setCapture(saved.capture);
}

void ReturnInferrer::inferReturnType(FuncSymbol *fn) {
  // A method of an instance of a generic class returns what the method of
  // the generic class returns, with the types of the instance; unless it
  // makes a class, which each instance makes its own (see isTemplate).
  if (FunctionDecl *decl = fn->decl; decl && decl->generic && decl->cls && decl->cls->generic &&
                                     !checker_.lateClasses().hasClassExpr(decl->generic)) {
    const Type *ret = returnType(decl->generic->sym);
    fn->ret = ret ? checker_.typeParams().substitute(
                        checker_.genericClassChecker().instanceType(decl->cls, ret),
                        decl->generic->sym->typeParams, fn->typeParams)
                  : Type::error();
    return;
  }
  // A method that takes the type of the one it overrides.
  if (const Type *inherited = checker_.classLinks().inheritedReturnType(fn)) {
    fn->ret = inherited;
    return;
  }
  FunctionDecl *decl = fn->decl;
  Checker::BodyState saved = enterBody(decl);
  // Real diagnostics, also when a trial in a cycle calls this function;
  // unless this is a function inside a copy that the trial checks.
  bool muted = checker_.diags_.setMuted(decl->isClosure() && saved.trial);
  if (!decl->isClosure() && !checker_.topLevelDone_) {
    checker_.early_ = fn; // top-level code calls it: globals declared later are not there yet
  } else if (decl->group && !decl->group->finished) {
    checker_.early_ = fn; // the same for the variables declared later in its block
  }

  if (auto cycle = checker_.cycleOf_.find(fn); cycle != checker_.cycleOf_.end()) {
    inferCycle(checker_.cycles_[cycle->second]);
  } else {
    checkAndInfer(decl);
  }

  checker_.diags_.setMuted(muted);
  leaveBody(saved);
}

void ReturnInferrer::checkAndInfer(FunctionDecl *decl) {
  FuncSymbol *fn = decl->sym;
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  std::vector<ReturnStmt *> *outer = checker_.returns_;
  std::vector<YieldExpr *> *outerYields = checker_.yields_;
  checker_.returns_ = &returns;
  checker_.yields_ = &yields;
  fn->inferring = true;
  checker_.inferring_.push_back(fn);
  checker_.functionChecker().checkFunctionBody(decl);
  checker_.inferring_.pop_back();
  fn->inferring = false;
  checker_.returns_ = outer;
  checker_.yields_ = outerYields;
  bool fallsOff = !ReturnChecker::alwaysReturns(decl->body);
  const Type *type =
      decl->isGenerator ? checker_.generatorChecker().inferGeneratorType(fn, yields, returns,
                                                                         fallsOff, /*report=*/true)
      : decl->isAsync
          ? checker_.asyncChecker().inferAsyncType(fn, returns, fallsOff, /*report=*/true)
          : unifyReturns(fn, returns, fallsOff, /*report=*/true);
  // The generator of safeTry also returns the errors of its `yield*`.
  if (decl->safeTry && type && !type->isError()) {
    type = checker_.safeTryChecker().generatorResult(decl, type);
  }
  fn->ret = type ? type : Type::error();
  if (decl->isAsync && !decl->isGenerator) {
    checker_.asyncChecker().finishAsyncReturns(fn, returns);
    return;
  }
  // Now every return converts its value to that type, and in a generator
  // every yield to the type of the values it gives; in an async generator,
  // what they await converts once it is there (see emitAsyncYield).
  bool async = decl->isAsync;
  const Type *returned = decl->isGenerator && (fn->ret->isIterator() || fn->ret->isAsyncIterator())
                             ? fn->ret->returned()
                             : fn->ret;
  if (!returned->isError() && !returned->isVoid()) {
    for (ReturnStmt *ret : returns) {
      if (ret->value && !ret->value->type->isError() &&
          (!async || (AsyncChecker::awaitedType(ret->value->type) == ret->value->type &&
                      !ret->value->type->isUndefined()))) {
        ret->value = checker_.typeRelations().convert(ret->value, returned);
      }
    }
  }
  if (fn->ret->isIterator() || fn->ret->isAsyncIterator()) {
    for (YieldExpr *yield : yields) {
      if (!yield->delegate && yield->value && !yield->value->type->isError() &&
          (!async || AsyncChecker::awaitedType(yield->value->type) == yield->value->type)) {
        yield->value = checker_.typeRelations().convert(yield->value, fn->ret->yielded());
      }
    }
  }
}

// Each function of the cycle is checked on a fresh copy of its body, with
// diagnostics off; a call within the cycle returns the type found so far for
// its callee, at first none (an error type that the other branch of `?:` or
// `??` overrides). The types grow until they stop changing, which takes a few
// rounds: a type only widens, from int to long to double, or to `T |
// undefined`. The bodies themselves are checked afterwards like any other,
// with the types found.
void ReturnInferrer::inferCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (FuncSymbol *fn : cycle) {
    fn->inferring = true;
  }
  checker_.inferring_.insert(checker_.inferring_.end(), cycle.begin(), cycle.end());
  bool muted = checker_.diags_.setMuted(true);
  bool failed = false;
  for (int round = 0; round < 8 && !failed; ++round) {
    bool changed = false;
    for (FuncSymbol *fn : cycle) {
      FunctionDecl *copy = freshCopy(fn->decl);
      std::vector<ReturnStmt *> returns;
      std::vector<YieldExpr *> yields;
      checker_.returns_ = &returns;
      checker_.yields_ = &yields;
      checker_.trial_ = true;
      checker_.flow_ = Checker::Flow();
      checker_.functionChecker().checkFunctionBody(copy);
      checker_.trial_ = false;
      checker_.returns_ = nullptr;
      checker_.yields_ = nullptr;
      bool fallsOff = !ReturnChecker::alwaysReturns(copy->body);
      const Type *found;
      if (fn->decl->isGenerator) {
        found = checker_.generatorChecker().inferGeneratorType(fn, yields, returns, fallsOff,
                                                               /*report=*/false);
      } else if (fn->decl->isAsync) {
        found = checker_.asyncChecker().inferAsyncType(fn, returns, fallsOff, /*report=*/false);
      } else {
        found = unifyReturns(fn, returns, fallsOff, /*report=*/false);
      }
      if (!found) {
        continue; // nothing known yet
      }
      const Type *next = fn->assumed ? LiteralTypes::unionUnify(fn->assumed, found) : found;
      if (!next || next->isError()) {
        failed = true;
        break;
      }
      if (next != fn->assumed) {
        fn->assumed = next;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  checker_.diags_.setMuted(muted);
  checker_.inferring_.resize(checker_.inferring_.size() - cycle.size());

  for (FuncSymbol *fn : cycle) {
    fn->inferring = false;
    fn->ret = fn->assumed && !failed ? fn->assumed : Type::error();
    fn->assumed = nullptr;
  }
  for (FuncSymbol *fn : cycle) {
    if (!fn->ret->isError()) {
      continue;
    }
    if (failed) {
      checker_.diags_.error(fn->loc, quotedName(fn->name) +
                                         " returns values of different types; a function "
                                         "returns values of one type");
    } else {
      checker_.diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) +
                                         " returns: every value it returns comes from " +
                                         (cycle.size() == 1 ? "a call to itself"
                                                            : "the functions that call it back") +
                                         "; declare its return type, e.g. '): int'");
    }
  }
}

const Type *ReturnInferrer::unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                         bool fallsOff, bool report) {
  const Type *type = nullptr;
  bool undefined = fallsOff;
  for (ReturnStmt *ret : returns) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *t = ret->value->type;
    if (t->isError()) {
      continue;
    }
    if (t->isUndefined()) {
      undefined = true;
      continue;
    }
    if (!type) {
      type = t;
      continue;
    }
    const Type *common = LiteralTypes::unionUnify(type, t);
    if (!common || common->isError()) {
      if (report) {
        checker_.diags_.error(ret->value->loc(),
                              quotedName(fn->name) + " returns '" + t->name() + "' here and '" +
                                  type->name() +
                                  "' elsewhere; a function returns values of one type");
      }
      return Type::error();
    }
    type = common;
  }
  if (!type) {
    if (report && undefined &&
        llvm::any_of(returns, [](const ReturnStmt *r) { return r->value; })) {
      checker_.diags_.error(fn->loc, quotedName(fn->name) +
                                         " returns only undefined; declare the return type, "
                                         "e.g. '): int | undefined'");
    }
    return report ? Type::error() : nullptr;
  }
  if (undefined && !type->isVoid()) {
    type = Type::optional(type);
  }

  return type;
}

// The parameters whose type comes from a default value that is not a
// literal: its type, found by checking it on a copy, in a scope with the
// parameters before it.
void ReturnInferrer::resolveParams(FuncSymbol *fn) {
  if (!fn->pendingParams) {
    return;
  }
  fn->pendingParams = false;
  FunctionDecl *decl = fn->decl;
  // Those of a method of an instance of a generic class: of the method of
  // the generic class, with the types of the instance.
  if (FunctionDecl *generic = decl->generic; generic && decl->cls && decl->cls->generic) {
    resolveParams(generic->sym);
    auto sub = [&](const Type *t) {
      return t ? checker_.typeParams().substitute(
                     checker_.genericClassChecker().instanceType(decl->cls, t),
                     generic->sym->typeParams, fn->typeParams)
               : nullptr;
    };
    for (size_t i = 0; i < fn->params.size() && i < generic->sym->params.size(); ++i) {
      fn->params[i] = sub(generic->sym->params[i]);
    }
    for (size_t i = 0; i < decl->params.size() && i < generic->params.size(); ++i) {
      decl->params[i].declaredType = sub(generic->params[i].declaredType);
    }
    return;
  }
  std::vector<const Type *> found(decl->params.size(), nullptr);
  // Muted at first; if a value has an error, a second check reports it.
  for (bool report : {false, true}) {
    if (report && llvm::none_of(found, [](const Type *t) { return t && t->isError(); })) {
      break;
    }
    Checker::BodyState saved = enterBody(decl);
    bool muted = checker_.diags_.setMuted(!report);
    FunctionDecl *copy = freshCopy(decl);
    // A method's default values see its `this` and the private members of
    // its class.
    checker_.functions_.push_back(copy);
    if (copy->takesThis()) {
      checker_.classChecker().declareThis(copy);
    }
    ClassDecl *outerClass = checker_.currentClass_;
    if (!copy->isClosure()) {
      checker_.currentClass_ = copy->cls;
    }
    checker_.pushScope();
    for (size_t i = 0; i < decl->params.size() && i < copy->params.size(); ++i) {
      const Type *type = decl->params[i].declaredType;
      if (!type) {
        type = found[i] = checker_.exprChecker().checkExpr(copy->params[i].defaultValue)->type;
      }
      checker_.scopes().declareVar(copy->params[i].name, copy->params[i].loc, type,
                                   /*isConst=*/false);
    }
    checker_.popScope();
    checker_.functions_.pop_back();
    checker_.currentClass_ = outerClass;
    checker_.diags_.setMuted(muted);
    leaveBody(saved);
  }
  for (size_t i = 0; i < decl->params.size(); ++i) {
    Param &param = decl->params[i];
    if (param.declaredType) {
      continue;
    }
    const Type *type = found[i] ? found[i] : Type::error();
    if (type->isVoid() || type->isUndefined()) {
      checker_.diags_.error(param.loc, "cannot tell the type of '" + param.label() +
                                           "' from its default value; "
                                           "add one, e.g. '" +
                                           param.label() + ": int | undefined = undefined'");
      type = Type::error();
    }
    // An error in the value itself is reported when the body is checked.
    param.declaredType = type;
    fn->params[i] = Type::optional(type);
  }
}

FunctionDecl *ReturnInferrer::freshCopy(const FunctionDecl *fn) {
  Lexer lexer(fn->source, checker_.diags_);
  Parser parser(lexer, checker_.ctx_, checker_.diags_, /*library=*/fn->cls && fn->cls->library);
  FunctionDecl *copy = fn->isArrow || ((fn->isExpression || fn->lifted) && !fn->cls)
                           ? parser.parseFunctionExprCopy()
                       : fn->cls ? parser.parseMethodCopy()
                                 : parser.parseFunctionCopy();
  copy->isExpression = fn->isExpression;
  copy->isAsync = fn->isAsync;
  copy->lifted = fn->lifted;
  copy->cls = fn->cls;
  copy->isStatic = fn->isStatic;
  copy->isConstructor = fn->isConstructor;
  copy->accessor = fn->accessor;
  copy->hiddenGetter = fn->hiddenGetter;
  copy->access = fn->access;
  copy->sym = fn->sym;
  copy->name = fn->name;
  copy->typeArgs = fn->typeArgs;
  copy->isInstance = fn->isInstance;
  copy->parent = fn->parent;
  copy->group = fn->group;
  copy->var = fn->var;
  copy->creationSeq = fn->creationSeq;
  // The parameters of an argument list in the place of the rest parameter
  // (see expandArgumentList).
  if (!fn->listName.empty() && copy->params.size() > fn->listStart) {
    copy->params.resize(fn->listStart);
    copy->listName = fn->listName;
    copy->listStart = fn->listStart;
    copy->listType = fn->listType;
  }
  for (size_t i = 0; i < copy->params.size() && i < fn->params.size(); ++i) {
    copy->params[i].declaredType = fn->params[i].declaredType;
  }
  // The parameters an arrow takes without naming them (see Signature).
  for (size_t i = copy->params.size(); i < fn->params.size(); ++i) {
    copy->params.push_back(fn->params[i]);
  }

  return copy;
}

} // namespace tz
