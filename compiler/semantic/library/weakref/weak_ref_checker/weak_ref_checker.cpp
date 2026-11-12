#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/weakref/weak_ref_method_table/weak_ref_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"

namespace tz {

WeakRefChecker::WeakRefChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

WeakRefChecker::ResultTargetIsWritten WeakRefChecker::checkTargetWritten(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != 1) {
    auto message = ErrorTextWeakRef::noTarget();

    return err(WeakRefCheckerTargetWrittenError{
        .code = WeakRefCheckerTargetWrittenErrorCode::NO_TARGET_TYPE,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

WeakRefChecker::ResultWeakTargetValid WeakRefChecker::checkWeakTarget(const Type *target) const {
  bool isUnsupported = target->isRegExp() || target->isMatch() || target->isDate() ||
                       target->isDateFormat() || target->isBytes() || target->isTimer();
  if (isUnsupported) {
    auto message = ErrorTextWeakTarget::notYet(target->name());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }
  bool isObject = target->isObject() && !target->isAnyObject() && !target->isWeakRef();
  if (!isObject) {
    auto message = ErrorTextWeakTarget::notObject(target->name(), target->isOptional());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(target);
}

WeakRefChecker::ResultNewTakesOneArgs WeakRefChecker::checkNewArgs(const NewExpr *expr) const {
  if (expr->args.size() != 1) {
    auto message = ErrorTextWeakRef::oneObject();

    return err(WeakRefCheckerNewArgsError{
        .code = WeakRefCheckerNewArgsErrorCode::NOT_ONE_OBJECT,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultNewTypeArgsFits WeakRefChecker::checkNewTypeArgs(const NewExpr *expr) const {
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextWeakRef::typeArgs();

    return err(WeakRefCheckerNewTypeArgsError{
        .code = WeakRefCheckerNewTypeArgsErrorCode::SEVERAL_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultMethodIsDerefed WeakRefChecker::checkMethodName(const MemberExpr *member,
                                                                      const Type *type) const {
  auto method = WeakRefMethodTable::find(member->name);
  if (method != WeakRefMethod::DEREF) {
    auto message = ErrorTextWeakRef::noMethod(member->name, type->name());

    return err(WeakRefCheckerMethodNameError{
        .code = WeakRefCheckerMethodNameErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(member);
}

const Type *WeakRefChecker::resolveWeakRefType(const TypeRef &ref) {
  auto written = this->checkTargetWritten(ref);
  if (written.isErr()) {
    this->state.getDiagnostics().error(ref.loc, written.error().message);
    return Type::error();
  }
  const TypeRef &targetRef = *written.value();
  const auto *target = this->typeResolver->resolveType(targetRef);
  if (target->isError()) {
    return target;
  }
  auto object = this->checkWeakTarget(target);
  if (object.isErr()) {
    this->state.getDiagnostics().error(targetRef.loc, object.error().message);
    return Type::error();
  }

  return Type::weakRef(target);
}

Expr *WeakRefChecker::checkWeakRefNew(NewExpr *expr, const Type *expected) {
  llvm::SMLoc loc = expr->loc();
  auto weakRefName = GlobalNameTable::getName(GlobalName::WEAK_REF);
  auto *callee = this->state.getAst().make<NameExpr>(loc, weakRefName);
  auto args = this->checkNewArgs(expr);
  if (args.isErr()) {
    this->state.getDiagnostics().error(loc, args.error().message);
    return this->failWeakRefNew(expr);
  }
  const auto *written = this->writtenTarget(expr, expected);
  if (written != nullptr && written->isError()) {
    return this->failWeakRefNew(expr);
  }
  auto *checked = this->exprChecker->checkExpr(expr->args[0], written);
  auto *arg = this->genericChecker->apparent(checked);
  if (arg->type->isError()) {
    return arg;
  }
  const auto *target = written != nullptr ? written : arg->type;
  auto object = this->checkWeakTarget(target);
  if (object.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), object.error().message);
    return arg;
  }
  auto *coerced = this->typeRelations->coerce(arg, target);
  const auto *weakRefType = Type::weakRef(target);

  return this->processChecker->builtinCall(loc, callee, Builtin::WeakRefNew, {coerced},
                                           weakRefType);
}

const Type *WeakRefChecker::writtenTarget(const NewExpr *expr, const Type *expected) {
  auto typeArgs = this->checkNewTypeArgs(expr);
  if (typeArgs.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), typeArgs.error().message);
    return nullptr;
  }
  if (expr->typeArgs.size() == 1) {
    return this->typeResolver->resolveType(expr->typeArgs[0]);
  }
  if (expected != nullptr && expected->nonOptional()->isWeakRef()) {
    return expected->nonOptional()->weakTarget();
  }

  return nullptr;
}

Expr *WeakRefChecker::failWeakRefNew(NewExpr *expr) {
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  auto *error = this->state.getAst().make<UndefinedExpr>(expr->loc());
  error->type = Type::error();

  return error;
}

Expr *WeakRefChecker::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  auto deref = this->checkMethodName(member, type);
  if (deref.isErr()) {
    this->state.getDiagnostics().error(member->loc(), deref.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::WeakRefDeref};
  call->type = Type::optional(type->weakTarget());

  return call;
}

}
