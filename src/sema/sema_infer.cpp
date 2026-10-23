// Return types inferred from function bodies: a function without a return
// type annotation returns the common type of what its `return` statements
// give, with undefined if it may also end without a value.
//
// A function that calls no function of unknown type is checked once, when its
// type is first needed. Functions that call each other (and a function that
// calls itself) need each other's types: inferCycle finds them together.

#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/sema.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool Sema::returnsValue(const Stmt *stmt) {
  if (!stmt)
    return false;
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    return cast<ReturnStmt>(stmt)->value != nullptr;
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
  default:
    return false;
  }
}

const Type *Sema::returnType(FuncSymbol *fn) {
  if (fn->ret)
    return fn->ret;
  if (fn->inferring) // a call in a cycle being inferred: the type so far
    return fn->assumed ? fn->assumed : Type::error();
  inferReturnType(fn);
  return fn->ret;
}

const Type *Sema::functionType(FuncSymbol *fn) {
  resolveParams(fn);
  const Type *ret = returnType(fn);
  return Type::function(fn->params, fn->minArgs, fn->hasRest, ret ? ret : Type::error());
}

Sema::BodyState Sema::enterBody(const FunctionDecl *fn) {
  BodyState saved;
  saved.flow = std::move(flow_);
  saved.scopes = scopes_;
  saved.typeScopes = typeScopes_;
  saved.functions = functions_;
  saved.groups = groups_;
  saved.blockNames = blockNames_;
  saved.function = currentFunction_;
  saved.targets = std::move(targets_);
  saved.pendingLabels = std::move(pendingLabels_);
  saved.otherCaseVars = otherCaseVars_;
  saved.chainShortCircuits = chainShortCircuits_;
  saved.returns = returns_;
  saved.trial = trial_;
  saved.early = early_;
  flow_ = Flow();
  if (NestedGroup *group = fn ? fn->group : nullptr) {
    // A function declared in a block sees what is visible in the block,
    // wherever the check started.
    scopes_.resize(group->scopeDepth);
    typeScopes_.resize(group->scopeDepth);
    functions_.resize(group->functionDepth);
    while (!groups_.empty() && groups_.back() != group)
      groups_.pop_back();
    while (!blockNames_.empty() && blockNames_.back().functions > group->functionDepth)
      blockNames_.pop_back();
  } else if (!fn || !fn->isExpression) {
    // A top-level function sees only globals.
    scopes_.resize(1);
    typeScopes_.resize(1);
    functions_.clear();
    groups_.clear();
    blockNames_.clear();
    otherCaseVars_.clear();
    trial_ = false;
  }
  // An arrow is checked where it is written.
  currentFunction_ = nullptr;
  targets_.clear();
  pendingLabels_.clear();
  chainShortCircuits_ = false;
  returns_ = nullptr;
  return saved;
}

void Sema::leaveBody(BodyState &saved) {
  flow_ = std::move(saved.flow);
  scopes_ = std::move(saved.scopes);
  typeScopes_ = std::move(saved.typeScopes);
  functions_ = std::move(saved.functions);
  groups_ = std::move(saved.groups);
  blockNames_ = std::move(saved.blockNames);
  currentFunction_ = saved.function;
  targets_ = std::move(saved.targets);
  pendingLabels_ = std::move(saved.pendingLabels);
  otherCaseVars_ = std::move(saved.otherCaseVars);
  chainShortCircuits_ = saved.chainShortCircuits;
  returns_ = saved.returns;
  trial_ = saved.trial;
  early_ = saved.early;
}

void Sema::inferReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  BodyState saved = enterBody(decl);
  // Real diagnostics, also when a trial in a cycle calls this function;
  // unless this is a function inside a copy that the trial checks.
  bool muted = diags_.setMuted(decl->isClosure() && saved.trial);
  if (!decl->isClosure() && !topLevelDone_)
    early_ = fn; // top-level code calls it: globals declared later are not there yet
  else if (decl->group && !decl->group->finished)
    early_ = fn; // the same for the variables declared later in its block

  if (auto cycle = cycleOf_.find(fn); cycle != cycleOf_.end())
    inferCycle(cycles_[cycle->second]);
  else
    checkAndInfer(decl);

  diags_.setMuted(muted);
  leaveBody(saved);
}

void Sema::checkAndInfer(FunctionDecl *decl) {
  FuncSymbol *fn = decl->sym;
  std::vector<ReturnStmt *> returns;
  std::vector<ReturnStmt *> *outer = returns_;
  returns_ = &returns;
  fn->inferring = true;
  checkFunctionBody(decl);
  fn->inferring = false;
  returns_ = outer;
  const Type *type = unifyReturns(fn, returns, !alwaysReturns(decl->body), /*report=*/true);
  fn->ret = type ? type : Type::error();
  // Now every return converts its value to that type.
  if (!fn->ret->isError() && !fn->ret->isVoid())
    for (ReturnStmt *ret : returns)
      if (ret->value && !ret->value->type->isError())
        ret->value = convert(ret->value, fn->ret);
}

// Each function of the cycle is checked on a fresh copy of its body, with
// diagnostics off; a call within the cycle returns the type found so far for
// its callee, at first none (an error type that the other branch of `?:` or
// `??` overrides). The types grow until they stop changing, which takes a few
// rounds: a type only widens, from int to long to double, or to `T |
// undefined`. The bodies themselves are checked afterwards like any other,
// with the types found.
void Sema::inferCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (FuncSymbol *fn : cycle)
    fn->inferring = true;
  bool muted = diags_.setMuted(true);
  bool failed = false;
  for (int round = 0; round < 8 && !failed; ++round) {
    bool changed = false;
    for (FuncSymbol *fn : cycle) {
      FunctionDecl *copy = freshCopy(fn->decl);
      std::vector<ReturnStmt *> returns;
      returns_ = &returns;
      trial_ = true;
      flow_ = Flow();
      checkFunctionBody(copy);
      trial_ = false;
      returns_ = nullptr;
      const Type *found = unifyReturns(fn, returns, !alwaysReturns(copy->body), /*report=*/false);
      if (!found)
        continue; // nothing known yet
      const Type *next = fn->assumed ? unify(fn->assumed, found) : found;
      if (!next || next->isError()) {
        failed = true;
        break;
      }
      if (next != fn->assumed) {
        fn->assumed = next;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
  diags_.setMuted(muted);

  for (FuncSymbol *fn : cycle) {
    fn->inferring = false;
    fn->ret = fn->assumed && !failed ? fn->assumed : Type::error();
    fn->assumed = nullptr;
  }
  for (FuncSymbol *fn : cycle) {
    if (!fn->ret->isError())
      continue;
    if (failed)
      diags_.error(fn->loc, quotedName(fn->name) + " returns values of different types; a function "
                                             "returns values of one type");
    else
      diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) + " returns: every value it returns comes from " +
                                (cycle.size() == 1 ? "a call to itself" : "the functions that call it back") +
                                "; declare its return type, e.g. '): int'");
  }
}

const Type *Sema::unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff,
                               bool report) {
  const Type *type = nullptr;
  bool undefined = fallsOff;
  for (ReturnStmt *ret : returns) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *t = ret->value->type;
    if (t->isError())
      continue;
    if (t->isUndefined()) {
      undefined = true;
      continue;
    }
    if (!type) {
      type = t;
      continue;
    }
    const Type *common = unify(type, t);
    if (!common || common->isError()) {
      if (report)
        diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() + "' here and '" +
                                            type->name() + "' elsewhere; a function returns values of one type");
      return Type::error();
    }
    type = common;
  }
  if (!type) {
    if (report && undefined && llvm::any_of(returns, [](const ReturnStmt *r) { return r->value; }))
      diags_.error(fn->loc, quotedName(fn->name) + " returns only undefined; declare the return type, "
                                             "e.g. '): int | undefined'");
    return report ? Type::error() : nullptr;
  }
  if (undefined && !type->isVoid())
    type = Type::optional(type);
  return type;
}

// The parameters whose type comes from a default value that is not a
// literal: its type, found by checking it on a copy, in a scope with the
// parameters before it.
void Sema::resolveParams(FuncSymbol *fn) {
  if (!fn->pendingParams)
    return;
  fn->pendingParams = false;
  FunctionDecl *decl = fn->decl;
  std::vector<const Type *> found(decl->params.size(), nullptr);
  // Muted at first; if a value has an error, a second check reports it.
  for (bool report : {false, true}) {
    if (report && llvm::none_of(found, [](const Type *t) { return t && t->isError(); }))
      break;
    BodyState saved = enterBody(decl);
    bool muted = diags_.setMuted(!report);
    FunctionDecl *copy = freshCopy(decl);
    pushScope();
    for (size_t i = 0; i < decl->params.size() && i < copy->params.size(); ++i) {
      const Type *type = decl->params[i].declaredType;
      if (!type)
        type = found[i] = checkExpr(copy->params[i].defaultValue)->type;
      declareVar(copy->params[i].name, copy->params[i].loc, type, /*isConst=*/false);
    }
    popScope();
    diags_.setMuted(muted);
    leaveBody(saved);
  }
  for (size_t i = 0; i < decl->params.size(); ++i) {
    Param &param = decl->params[i];
    if (param.declaredType)
      continue;
    const Type *type = found[i] ? found[i] : Type::error();
    if (type->isVoid() || type->isUndefined()) {
      diags_.error(param.loc, "cannot tell the type of '" + param.name + "' from its default value; "
                                                                        "add one, e.g. '" + param.name +
                                  ": int | undefined = undefined'");
      type = Type::error();
    }
    // An error in the value itself is reported when the body is checked.
    param.declaredType = type;
    fn->params[i] = Type::optional(type);
  }
}

FunctionDecl *Sema::freshCopy(const FunctionDecl *fn) {
  Lexer lexer(fn->source, diags_);
  Parser parser(lexer, ctx_, diags_);
  FunctionDecl *copy = fn->isExpression ? parser.parseFunctionExprCopy() : parser.parseFunctionCopy();
  copy->sym = fn->sym;
  copy->name = fn->name;
  copy->parent = fn->parent;
  copy->group = fn->group;
  copy->var = fn->var;
  copy->creationSeq = fn->creationSeq;
  for (size_t i = 0; i < copy->params.size() && i < fn->params.size(); ++i)
    copy->params[i].declaredType = fn->params[i].declaredType;
  // The parameters an arrow takes without naming them (see Signature).
  for (size_t i = copy->params.size(); i < fn->params.size(); ++i)
    copy->params.push_back(fn->params[i]);
  return copy;
}

} // namespace tz
