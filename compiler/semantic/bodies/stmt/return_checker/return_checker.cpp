#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

class Exits {
public:
  Exits(bool forLoop, llvm::ArrayRef<llvm::StringRef> ownLabels);

  bool any(llvm::ArrayRef<Stmt *> stmts);
  bool in(const Stmt *stmt);

private:
  bool inLoop(const Stmt *body);

  bool isLoop;
  llvm::ArrayRef<llvm::StringRef> labels;
  llvm::SmallVector<llvm::StringRef, 4> inner;
  unsigned loops = 0;
  unsigned breakables = 0;
};

Exits::Exits(bool forLoop, llvm::ArrayRef<llvm::StringRef> ownLabels)
    : isLoop(forLoop),
      labels(ownLabels) {}

bool Exits::any(llvm::ArrayRef<Stmt *> stmts) {
  return llvm::any_of(stmts, [&](const Stmt *s) { return this->in(s); });
}

bool Exits::in(const Stmt *stmt) {
  switch (stmt->kind()) {
    case Stmt::Kind::Break: {
      llvm::StringRef label = cast<BreakStmt>(stmt)->label;
      return label.empty() ? this->breakables == 0 : !llvm::is_contained(this->inner, label);
    }
    case Stmt::Kind::Continue: {
      llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
      if (label.empty()) {
        return this->loops == 0 && !this->isLoop;
      }
      if (llvm::is_contained(this->inner, label)) {
        return false;
      }
      return !(this->isLoop && llvm::is_contained(this->labels, label));
    }
    case Stmt::Kind::Block:
      return this->any(cast<BlockStmt>(stmt)->body);
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      return this->in(s->thenStmt) || (s->elseStmt && this->in(s->elseStmt));
    }
    case Stmt::Kind::While:
      return this->inLoop(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return this->inLoop(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return this->inLoop(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return this->inLoop(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch: {
      ++this->breakables;
      bool found = llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                                [&](const SwitchCase &c) { return this->any(c.body); });
      --this->breakables;
      return found;
    }
    case Stmt::Kind::Labeled: {
      auto *s = cast<LabeledStmt>(stmt);
      this->inner.push_back(s->label);
      bool found = this->in(s->body);
      this->inner.pop_back();
      return found;
    }
    default:
      return false;
  }
}

bool Exits::inLoop(const Stmt *body) {
  ++this->loops;
  ++this->breakables;
  bool found = this->in(body);
  --this->loops;
  --this->breakables;

  return found;
}

}

bool ReturnChecker::isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);

  return lit && lit->value;
}

bool ReturnChecker::breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  if (labels.empty()) {
    return false;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Break:
    return llvm::is_contained(labels, cast<BreakStmt>(stmt)->label);
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body,
                        [&](const Stmt *s) { return ReturnChecker::breaksTo(s, labels); });
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return ReturnChecker::breaksTo(s->thenStmt, labels) ||
           (s->elseStmt && ReturnChecker::breaksTo(s->elseStmt, labels));
  }
  case Stmt::Kind::While:
    return ReturnChecker::breaksTo(cast<WhileStmt>(stmt)->body, labels);
  case Stmt::Kind::DoWhile:
    return ReturnChecker::breaksTo(cast<DoWhileStmt>(stmt)->body, labels);
  case Stmt::Kind::For:
    return ReturnChecker::breaksTo(cast<ForStmt>(stmt)->body, labels);
  case Stmt::Kind::ForOf:
    return ReturnChecker::breaksTo(cast<ForOfStmt>(stmt)->body, labels);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases, [&](const SwitchCase &c) {
      return llvm::any_of(c.body,
                          [&](const Stmt *s) { return ReturnChecker::breaksTo(s, labels); });
    });
  case Stmt::Kind::Labeled:
    return ReturnChecker::breaksTo(cast<LabeledStmt>(stmt)->body, labels);
  default:
    return false;
  }
}

ReturnChecker::ReturnChecker(SemanticState &owner)
    : state(owner) {}

Expr *ReturnChecker::checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested) {
  if (!this->state.getPredicateStore().isBuildingPredicate()) {
    return ExprChecker(this->state).checkExpr(ret->value, hint);
  }
  if (!tested) {
    Expr *value = ExprChecker(this->state).checkExpr(ret->value, hint);
    if (value->type->isBool()) {
      Flow whenTrue = this->state.getFlowState().getFlow();
      Flow whenFalse = this->state.getFlowState().getFlow();
      ConditionFacts(this->state).addConditionFacts(value, whenTrue, whenFalse);
      PredicateChecker::ResultExitNarrowed narrowed =
          PredicateChecker(this->state).notePredicateExit(whenTrue, whenFalse);
      if (narrowed.isErr()) {
        this->state.getDiagnostics().error(ret->value->loc(), narrowed.error().message);
      }
    }
    return value;
  }
  Flow whenTrue, whenFalse;
  Expr *value = TruthChecker(this->state).checkBranching(ret->value, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
  PredicateChecker::ResultExitNarrowed narrowed =
      PredicateChecker(this->state).notePredicateExit(whenTrue, whenFalse);
  if (narrowed.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), narrowed.error().message);
  }

  return value;
}

ReturnChecker::ResultInsideOfFunction ReturnChecker::enclosingFunction() const {
  if (!this->state.getFunctionStack().getCurrentFunction()) {
    std::string message = ErrorTextReturnPlace::outside();

    return err(ReturnCheckerEnclosingFunctionError{
        .code = ReturnCheckerEnclosingFunctionErrorCode::OUTSIDE_FUNCTION,
        .message = message,
    });
  }

  return ok(this->state.getFunctionStack().getCurrentFunction());
}

ReturnChecker::ResultConstructorExits ReturnChecker::constructorReturn(const ReturnStmt *ret,
                                                                       const FuncSymbol *fn) const {
  if (ret->value) {
    std::string message = ErrorTextConstructorReturn::value();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED,
        .message = message,
    });
  }
  bool initializer = this->state.getClassContext().getInitializer() == fn->decl;
  if (initializer && this->state.getFlowState().isBeforeSuper()) {
    std::string message = ErrorTextConstructorReturn::early();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (initializer) {
    ClassChecker::ResultEveryFieldSet fieldsSet = ClassChecker(this->state).checkFieldsSet();
    if (fieldsSet.isErr()) {
      return err(ReturnCheckerConstructorReturnError{
          .code = ReturnCheckerConstructorReturnErrorCode::FIELDS_UNSET,
          .message = fieldsSet.error().message,
      });
    }
  }

  return ok(fn);
}

ReturnChecker::ResultAwaitNotReturned ReturnChecker::awaitReturned(const ReturnStmt *ret,
                                                                   const FuncSymbol *fn) {
  bool awaited = fn->decl && fn->decl->isAsync && !fn->decl->isGenerator && ret->value &&
                 isa<AwaitExpr>(ret->value);
  if (awaited && fn->decl->concise()) {
    std::string message = ErrorTextReturnAwait::concise();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::CONCISE_AWAIT,
        .message = message,
    });
  }
  if (awaited) {
    std::string message = ErrorTextReturnAwait::returned();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::RETURN_AWAIT,
        .message = message,
    });
  }

  return ok(ret);
}

ReturnChecker::ResultValueNotRequired ReturnChecker::valueRequired(const Type *returned,
                                                                   const FuncSymbol *fn) {
  if (!returned->isVoid() && !returned->isError() && !returned->isOptional()) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::missing(label, returned->name());

    return err(ReturnCheckerValueRequiredError{
        .code = ReturnCheckerValueRequiredErrorCode::MISSING_VALUE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultGeneratorReturns
ReturnChecker::generatorReturn(const Type *given, const Type *returned, const FuncSymbol *fn) {
  bool wrong = given && !given->isUndefined() && !given->isVoid() &&
               (returned->isVoid() || !TypeRelations::isImplicitlyConvertible(given, returned));
  if (wrong) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::wrong(label, returned->name(), given->name());

    return err(ReturnCheckerGeneratorReturnError{
        .code = ReturnCheckerGeneratorReturnErrorCode::WRONG_TYPE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultPromiseTypeMatch
ReturnChecker::promiseReturn(const Type *given, const Type *returned, const FuncSymbol *fn) {
  if (given != returned && !given->isNever()) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::promise(given->name(), label, returned->name());

    return err(ReturnCheckerPromiseReturnError{
        .code = ReturnCheckerPromiseReturnErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

ReturnChecker::ResultVoidReturnsEmpty ReturnChecker::voidReturn(const Type *type,
                                                                const FuncSymbol *fn) {
  if (!type->isVoid() && !type->isError() && !type->isUndefined() && !fn->discardsResult) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::notVoid(label, type->name());

    return err(ReturnCheckerVoidReturnError{
        .code = ReturnCheckerVoidReturnErrorCode::VALUE_FROM_VOID,
        .message = message,
    });
  }

  return ok(type);
}

void ReturnChecker::checkReturn(ReturnStmt *ret) {
  ResultInsideOfFunction inside = this->enclosingFunction();
  if (inside.isErr()) {
    this->state.getDiagnostics().error(ret->loc(), inside.error().message);
    if (ret->value) {
      ret->value = ExprChecker(this->state).checkExpr(ret->value);
    }
    return;
  }

  EmitterChecker(this->state).reportUnlistenedErrors();
  const FuncSymbol *fn = inside.value();
  if (fn->decl && fn->decl->isConstructor()) {
    if (ret->value) {
      ret->value = ExprChecker(this->state).checkExpr(ret->value);
    }
    ResultConstructorExits exits = this->constructorReturn(ret, fn);
    if (exits.isErr()) {
      bool valued = exits.error().code == ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED;
      llvm::SMLoc at = valued ? ret->value->loc() : ret->loc();
      this->state.getDiagnostics().error(at, exits.error().message);
    }
    return;
  }
  ResultAwaitNotReturned awaited = ReturnChecker::awaitReturned(ret, fn);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), awaited.error().message);
  }
  const Type *returned = fn->hasReturnType() && fn->decl && fn->decl->isGenerator &&
                                 (fn->getKnownReturnType()->isIterator() ||
                                  fn->getKnownReturnType()->isAsyncIterator())
                             ? fn->getKnownReturnType()->returned()
                         : fn->hasReturnType() && fn->decl && fn->decl->isAsync &&
                                 fn->getKnownReturnType()->isPromise()
                             ? fn->getKnownReturnType()->promised()
                             : fn->getKnownReturnType();
  if (!fn->hasReturnType()) {
    const Type *hint = nullptr;
    if (ret->value && Destination::literalShaped(ret->value)) {
      for (const ReturnStmt *earlier : this->state.getFunctionStack().getCollectedReturns()) {
        if (earlier->value && !earlier->value->type->isError() &&
            (earlier->value->type->holdsObjects() || earlier->value->type->isResult())) {
          hint = earlier->value->type;
          break;
        }
      }
    }
    if (ret->value) {
      ret->value = this->checkReturnValue(ret, hint, PredicateChecker::looksBoolean(ret->value));
    }
    this->state.getFunctionStack().addCollectedReturn(ret);
    return;
  }
  if (!ret->value) {
    ResultValueNotRequired bare = ReturnChecker::valueRequired(returned, fn);
    if (bare.isErr()) {
      this->state.getDiagnostics().error(ret->loc(), bare.error().message);
    }
    return;
  }

  if (fn->decl && fn->decl->isAsync && fn->decl->isGenerator) {
    ret->value =
        ExprChecker(this->state).checkExpr(ret->value, returned->isVoid() ? nullptr : returned);
    const Type *given =
        ret->value->type->isError() ? nullptr : AsyncChecker::awaitedType(ret->value->type);
    ResultGeneratorReturns gives = ReturnChecker::generatorReturn(given, returned, fn);
    if (gives.isErr()) {
      this->state.getDiagnostics().error(ret->value->loc(), gives.error().message);
    }
    return;
  }
  ret->value =
      this->checkReturnValue(ret, returned->isVoid() ? nullptr : returned, returned->isBool());
  if (fn->decl && fn->decl->isAsync && fn->getKnownReturnType()->isPromise() &&
      ret->value->type->isPromise()) {
    const Type *given = ret->value->type->promised();
    ResultPromiseTypeMatch match = ReturnChecker::promiseReturn(given, returned, fn);
    if (match.isErr()) {
      this->state.getDiagnostics().error(ret->value->loc(), match.error().message);
    }
    return;
  }
  if (returned->isVoid()) {
    ResultVoidReturnsEmpty empty = ReturnChecker::voidReturn(ret->value->type, fn);
    if (empty.isErr()) {
      this->state.getDiagnostics().error(ret->value->loc(), empty.error().message);
      ClassLinks(this->state).noteInheritedReturn(fn->decl);
    } else if (fn->discardsResult) {
      ResultChecker::ResultValueGetsUsed used =
          ResultChecker(this->state).usedValue(ret->value, true);
      if (used.isErr()) {
        this->state.getDiagnostics().error(ret->value->loc(), used.error().message);
      }
    }
    return;
  }
  unsigned errors = this->state.getDiagnostics().errorCount();
  ret->value = TypeRelations(this->state).coerce(ret->value, returned);
  if (this->state.getDiagnostics().errorCount() > errors) {
    ClassLinks(this->state).noteInheritedReturn(fn->decl);
  } else if (fn->decl) {
    ByteProofs(this->state).checkByteCallbackResult(fn->decl, ret->value);
  }
}

bool ReturnChecker::isExit(const Stmt *stmt) {
  auto *s = dyn_cast<ExprStmt>(stmt);
  auto *call = s ? dyn_cast<CallExpr>(s->expr) : nullptr;

  return call && (call->builtin() == Builtin::ProcessExit || call->builtin() == Builtin::LibPanic);
}

bool ReturnChecker::alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  auto returns = [](const Stmt *s) { return ReturnChecker::alwaysReturns(s); };
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    return true;
  case Stmt::Kind::Expr:
    return ReturnChecker::isExit(stmt);
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, returns) &&
           !ReturnChecker::breaksTo(stmt, labels);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    if (auto *lit = dyn_cast<BoolLitExpr>(s->cond)) {
      if (lit->value && !s->elseStmt) {
        return ReturnChecker::alwaysReturns(s->thenStmt) && !ReturnChecker::breaksTo(stmt, labels);
      }
      if (!lit->value && isa<EmptyStmt>(s->thenStmt)) {
        return s->elseStmt && ReturnChecker::alwaysReturns(s->elseStmt) &&
               !ReturnChecker::breaksTo(stmt, labels);
      }
    }
    return s->elseStmt && ReturnChecker::alwaysReturns(s->thenStmt) &&
           ReturnChecker::alwaysReturns(s->elseStmt) && !ReturnChecker::breaksTo(stmt, labels);
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return ReturnChecker::isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return ReturnChecker::isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || ReturnChecker::isTrueLiteral(s->cond)) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    bool hasDefault =
        s->exhaustive || llvm::any_of(s->cases, [](const SwitchCase &c) { return !c.test; });
    if (!hasDefault || s->cases.empty()) {
      return false;
    }
    Exits exits(false, labels);
    if (llvm::any_of(s->cases, [&](const SwitchCase &c) { return exits.any(c.body); })) {
      return false;
    }
    return llvm::any_of(s->cases.back().body, returns);
  }
  case Stmt::Kind::Labeled: {
    llvm::SmallVector<llvm::StringRef, 2> all(labels.begin(), labels.end());
    while (auto *labeled = dyn_cast<LabeledStmt>(stmt)) {
      all.push_back(labeled->label);
      stmt = labeled->body;
    }
    return ReturnChecker::alwaysReturns(stmt, all);
  }
  default:
    return false;
  }
}

}
