#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kInternalPrefix = "%";
constexpr llvm::StringLiteral kResultPlaceholder = "r";

}

ResultChecker::ResultChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->resultTypes = &all.resultTypes;
  this->asyncChecker = &all.asyncChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->argumentChecker = &all.argumentChecker;
}

const Type *ResultChecker::halfParam(const Type *half) const {
  return half->isVoid() ? Type::undefinedTy() : half;
}

bool ResultChecker::isGivingType(const Type *half) const {
  return !half->isNever() && !half->isVoid();
}

const Type *ResultChecker::halfType(const Type *result, Half half) const {
  return half == Half::OK ? result->okType() : result->errType();
}

const Type *ResultChecker::otherHalfType(const Type *result, Half half) const {
  return half == Half::OK ? result->errType() : result->okType();
}

std::string ResultChecker::receiverText(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isConverted = conversion != nullptr && conversion->isImplicit();
  const Expr *bare = isConverted ? conversion->operand : expr;
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->name.starts_with(kInternalPrefix)) {
    return kResultPlaceholder.str();
  }

  return name->name.str();
}

bool ResultChecker::hasFittingArgs(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max) {
  if (this->callChecker->rejectSpread(call)) {
    return false;
  }
  auto arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return arity.isOk();
}

Expr *ResultChecker::checkResultCall(CallExpr *call, NameExpr *name, const Type *expected) {
  auto which = this->resultTypes->resultName(name->name);
  if (which == GlobalName::OK) {
    return this->checkOkErr(call, name, Half::OK, expected);
  }
  if (which == GlobalName::ERR) {
    return this->checkOkErr(call, name, Half::ERR, expected);
  }

  return nullptr;
}

ResultChecker::ResultOkErrTypeArgs ResultChecker::writtenResult(const NameExpr *name) {
  if (name->typeArgs.empty()) {
    return ok<const Type *>(nullptr);
  }
  if (name->typeArgs.size() != 2) {
    auto message = ErrorTextOkErr::typeArgs(name->name);

    return err(ResultCheckerOkErrTypeArgsError{
        .code = ResultCheckerOkErrTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *value = this->typeResolver->resolveTypeOrVoid(name->typeArgs[0]);
  const auto *error = this->typeResolver->resolveTypeOrVoid(name->typeArgs[1]);
  if (value->isError() || error->isError()) {
    return err(ResultCheckerOkErrTypeArgsError{
        .code = ResultCheckerOkErrTypeArgsErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  const auto *written = Type::result(value, error);

  return ok(written);
}

const Type *ResultChecker::expectedResult(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isResult()) {
    return nullptr;
  }

  return expected->nonOptional();
}

Expr *ResultChecker::checkOkErr(CallExpr *call, NameExpr *name, Half half, const Type *expected) {
  if (!this->hasFittingArgs(call, name->loc(), 0, 1)) {
    return call;
  }
  auto written = this->writtenResult(name);
  bool isShown = written.isErr() &&
                 written.error().code != ResultCheckerOkErrTypeArgsErrorCode::ALREADY_REPORTED;
  if (isShown) {
    this->state.getDiagnostics().error(name->loc(), written.error().message);
  }
  if (written.isErr()) {
    return this->markCallFailed(call);
  }
  const Type *wanted =
      written.value() != nullptr ? written.value() : this->expectedResult(expected);
  const Type *wantedHalf = wanted != nullptr ? this->halfType(wanted, half) : nullptr;
  const Type *given = this->checkHalfValue(call, name, wantedHalf);
  const Type *other = wanted != nullptr ? this->otherHalfType(wanted, half) : Type::never();
  if (half == Half::OK) {
    call->target = BuiltinCall{.builtin = Builtin::ResultOk};
    call->type = Type::result(given, other);

    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::ResultErr};
  call->type = Type::result(other, given);

  return call;
}

const Type *ResultChecker::checkHalfValue(CallExpr *call, const NameExpr *name,
                                          const Type *wantedHalf) {
  if (call->args.empty()) {
    return Type::voidTy();
  }
  bool isHinted = wantedHalf != nullptr && this->isGivingType(wantedHalf);
  auto *arg = this->exprChecker->checkExpr(call->args[0], isHinted ? wantedHalf : nullptr);
  call->args[0] = arg;
  const Type *given = arg->type;
  if (given->isVoid()) {
    auto message = ErrorTextOkErr::voidValue(name->name);
    this->state.getDiagnostics().error(arg->loc(), message);

    return Type::error();
  }
  if (isHinted && TypeRelations::isImplicitlyConvertible(given, wantedHalf)) {
    call->args[0] = this->typeRelations->convert(arg, wantedHalf);

    return wantedHalf;
  }
  if (name->typeArgs.size() == 2 && !given->isError()) {
    call->args[0] = this->typeRelations->coerce(arg, wantedHalf);

    return wantedHalf;
  }

  return given;
}

void ResultChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->looselyChecked(arg);
  }
}

Expr *ResultChecker::looselyChecked(Expr *arg) {
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr == nullptr) {
    return this->exprChecker->checkExpr(arg);
  }
  auto params = std::vector<const Type *>(functionExpr->fn->params.size(), Type::error());
  auto loose = Signature{
      .params = params,
      .pad = false,
  };

  return this->closureChecker->checkFunctionExpr(functionExpr, &loose);
}

Expr *ResultChecker::markCallFailed(CallExpr *call) {
  this->walkArgsLoosely(call);
  call->type = Type::error();

  return call;
}

ResultChecker::ResultStaticIsKnown ResultChecker::staticMethod(const NameExpr *name,
                                                               llvm::StringRef method) const {
  auto found = ResultStaticTable::find(method);
  if (!found.has_value()) {
    auto message = ErrorTextCombine::noStatic(name->name, method);

    return err(ResultCheckerStaticMethodError{
        .code = ResultCheckerStaticMethodErrorCode::NO_SUCH_STATIC_METHOD,
        .message = message,
    });
  }

  return ok(*found);
}

Expr *ResultChecker::checkResultStatic(CallExpr *call, MemberExpr *member) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (name == nullptr || call->optional || member->optional) {
    return nullptr;
  }
  auto which = this->resultTypes->resultName(name->name);
  if (which != GlobalName::RESULT) {
    return nullptr;
  }
  auto known = this->staticMethod(name, member->name);
  if (known.isErr()) {
    this->state.getDiagnostics().error(member->loc(), known.error().message);

    return this->markCallFailed(call);
  }
  if (!this->hasFittingArgs(call, member->loc(), 1, 1)) {
    return call;
  }
  bool isAll = known.value() == ResultStatic::COMBINE_WITH_ALL_ERRORS;
  Builtin builtin = isAll ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine;
  call->target = BuiltinCall{.builtin = builtin};
  auto combine = CombineCall{
      .call = call,
      .what = name->name.str() + "." + member->name.str(),
      .isAll = isAll,
  };
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (literal != nullptr && this->isPlainList(literal)) {
    return this->checkCombineList(combine, literal);
  }

  return this->checkCombineArray(combine);
}

bool ResultChecker::isPlainList(const ArrayLitExpr *literal) const {
  if (literal->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread || element.hole) {
      return false;
    }
  }

  return true;
}

bool ResultChecker::isHoldingValues(const CombineCall &combine, const Type *result) const {
  bool isLosingErrors = combine.isAll && result->errType()->isVoid();

  return this->isGivingType(result->okType()) && !isLosingErrors;
}

void ResultChecker::reportNoValues(const CombineCall &combine, const Type *result, const Expr *at) {
  bool hasValues = this->isGivingType(result->okType());
  auto message = ErrorTextCombine::holds(combine.what, hasValues, result->name());
  this->state.getDiagnostics().error(at->loc(), message);
}

const Type *ResultChecker::errorsOf(const CombineCall &combine, const Type *error) const {
  if (!combine.isAll || error->isNever()) {
    return error;
  }

  return Type::array(error);
}

Expr *ResultChecker::checkCombineList(const CombineCall &combine, ArrayLitExpr *literal) {
  CallExpr *call = combine.call;
  auto results = std::vector<const Type *>();
  for (ArrayLitExpr::Element &element : literal->elements) {
    element.expr = this->exprChecker->checkExpr(element.expr);
    results.push_back(element.expr->type);
  }
  for (const Type *type : results) {
    if (type->isError()) {
      return call;
    }
  }
  for (size_t index = 0; index < results.size(); ++index) {
    if (!results[index]->isResult()) {
      auto message = ErrorTextCombine::notResult(combine.what, results[index]->name());
      this->state.getDiagnostics().error(literal->elements[index].expr->loc(), message);

      return call;
    }
  }
  const auto *common = this->commonResult(results);
  if (common != nullptr && !common->isError()) {
    return this->checkSameCombine(combine, literal, common);
  }

  return this->checkTupleCombine(combine, literal, results);
}

const Type *ResultChecker::sameValues(const Type *first, const Type *second) const {
  const auto *value = this->joinedValue(first->okType(), second->okType());
  const auto *error = this->resultTypes->unifyHalf(first->errType(), second->errType());
  bool isJoined = value != nullptr && !value->isError() && error != nullptr;

  return isJoined ? Type::result(value, error) : nullptr;
}

const Type *ResultChecker::joinedValue(const Type *first, const Type *second) const {
  if (first == second || second->isNever()) {
    return first;
  }
  if (first->isNever()) {
    return second;
  }
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }

  return this->typeRelations->unify(first, second);
}

const Type *ResultChecker::commonResult(llvm::ArrayRef<const Type *> results) const {
  const Type *common = results[0];
  for (const Type *type : results) {
    if (common == nullptr) {
      return nullptr;
    }
    common = this->sameValues(common, type);
  }

  return common;
}

Expr *ResultChecker::checkSameCombine(const CombineCall &combine, ArrayLitExpr *literal,
                                      const Type *common) {
  CallExpr *call = combine.call;
  if (!this->isHoldingValues(combine, common)) {
    this->reportNoValues(combine, common, call->args[0]);

    return call;
  }
  for (ArrayLitExpr::Element &element : literal->elements) {
    element.expr = this->typeRelations->convert(element.expr, common);
  }
  literal->type = Type::array(common);
  const auto *values = Type::array(common->okType());
  const auto *errors = this->errorsOf(combine, common->errType());
  call->type = Type::result(values, errors);

  return call;
}

Expr *ResultChecker::checkTupleCombine(const CombineCall &combine, ArrayLitExpr *literal,
                                       const std::vector<const Type *> &results) {
  CallExpr *call = combine.call;
  auto values = std::vector<const Type *>();
  const auto *error = Type::never();
  for (size_t index = 0; index < results.size(); ++index) {
    const Type *result = results[index];
    const Expr *element = literal->elements[index].expr;
    if (!this->isHoldingValues(combine, result)) {
      this->reportNoValues(combine, result, element);

      return call;
    }
    values.push_back(result->okType());
    const auto *joined = this->resultTypes->unifyHalf(error, result->errType());
    if (joined == nullptr) {
      auto message = ErrorTextCombine::noUnion(combine.what, error, result->errType());
      this->state.getDiagnostics().error(element->loc(), message);

      return call;
    }
    error = joined;
  }
  literal->type = Type::tuple(results);
  const auto *tuple = Type::tuple(values);
  const auto *errors = this->errorsOf(combine, error);
  call->type = Type::result(tuple, errors);

  return call;
}

Expr *ResultChecker::checkCombineArray(const CombineCall &combine) {
  CallExpr *call = combine.call;
  auto *list = this->exprChecker->checkExpr(call->args[0]);
  call->args[0] = list;
  const Type *type = list->type;
  if (type->isError()) {
    return call;
  }
  if (!type->isArray() || !type->inner()->isResult()) {
    auto message = ErrorTextCombine::notArray(combine.what, type->name());
    this->state.getDiagnostics().error(list->loc(), message);

    return call;
  }
  const Type *element = type->inner();
  if (!this->isHoldingValues(combine, element)) {
    this->reportNoValues(combine, element, list);

    return call;
  }
  const auto *values = Type::array(element->okType());
  const auto *errors = this->errorsOf(combine, element->errType());
  call->type = Type::result(values, errors);

  return call;
}

Expr *ResultChecker::checkResultMember(MemberExpr *member) {
  auto property = BuiltinPropertyTable::find(member->name);
  if (property == BuiltinProperty::RESULT_VALUE) {
    return this->checkHalfMember(member, Half::OK);
  }
  if (property == BuiltinProperty::RESULT_ERROR) {
    return this->checkHalfMember(member, Half::ERR);
  }
  auto receiver = this->receiverText(member->object);
  const auto *method = ResultMethodTable::find(member->name);
  std::string message =
      method != nullptr
          ? ErrorTextResultMember::method(member->name, receiver)
          : ErrorTextResultMember::noMember(member->name, member->object->type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *ResultChecker::checkHalfMember(MemberExpr *member, Half half) {
  const Type *type = member->object->type;
  if (this->otherHalfType(type, half)->isNever()) {
    const auto *own = this->halfType(type, half);
    member->type = this->halfParam(own);

    return this->definedHalf(member);
  }
  auto receiver = this->receiverText(member->object);
  std::string message = half == Half::OK ? ErrorTextResultMember::value(receiver)
                                         : ErrorTextResultMember::error(receiver);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *ResultChecker::definedHalf(MemberExpr *half) {
  auto *narrowed = this->fieldFacts->narrowedField(half);
  if (narrowed != half) {
    return narrowed;
  }
  if (!half->type->isOptional()) {
    return half;
  }
  auto path = this->fieldFacts->fieldPath(half);
  if (path.has_value() && this->fieldFacts->isFieldDefined(*path)) {
    return this->typeRelations->convert(half, half->type->inner());
  }

  return half;
}

ResultChecker::ResultValueGetsUsed ResultChecker::usedValue(const Expr *expr) {
  return this->checkUsed(expr, Discarded::BY_STATEMENT, llvm::SMLoc());
}

ResultChecker::ResultValueGetsUsed ResultChecker::usedValue(const Expr *expr, llvm::SMLoc at) {
  return this->checkUsed(expr, Discarded::BY_STATEMENT, at);
}

ResultChecker::ResultValueGetsUsed ResultChecker::usedCallbackValue(const Expr *expr) {
  return this->checkUsed(expr, Discarded::BY_CALLBACK, llvm::SMLoc());
}

ResultChecker::ResultValueGetsUsed ResultChecker::checkUsed(const Expr *expr, Discarded how,
                                                            llvm::SMLoc at) {
  const Type *type = expr->type;
  if (type != nullptr && type->holdsPromises()) {
    return this->checkPromiseUsed(expr, how, at);
  }
  if (type == nullptr || !type->holdsResults() || isa<AssignExpr>(expr)) {
    return ok(expr);
  }
  if (this->asyncChecker->isTakingElement(expr)) {
    return ok(expr);
  }
  bool isResult = type->nonOptional()->isResult();
  if (!isResult && this->asyncChecker->isMappingCall(expr)) {
    auto message = ErrorTextUnusedResult::results();

    return err(ResultCheckerUsedValueError{
        .code = ResultCheckerUsedValueErrorCode::RESULTS_UNUSED,
        .message = message,
    });
  }
  if (!isResult) {
    return ok(expr);
  }
  if (how == Discarded::BY_CALLBACK) {
    auto message = ErrorTextUnusedResult::callback();

    return err(ResultCheckerUsedValueError{
        .code = ResultCheckerUsedValueErrorCode::CALLBACK_RESULT_DROPPED,
        .message = message,
    });
  }
  auto message = ErrorTextUnusedResult::result();

  return err(ResultCheckerUsedValueError{
      .code = ResultCheckerUsedValueErrorCode::RESULT_UNUSED,
      .message = message,
  });
}

ResultChecker::ResultValueGetsUsed ResultChecker::checkPromiseUsed(const Expr *expr, Discarded how,
                                                                   llvm::SMLoc at) {
  bool isCallback = how == Discarded::BY_CALLBACK;
  auto handled = this->asyncChecker->checkDiscardedPromise(expr, isCallback, at);
  if (handled.isErr()) {
    return err(ResultCheckerUsedValueError{
        .code = ResultCheckerUsedValueErrorCode::PROMISE_DROPPED,
        .message = handled.error().message,
    });
  }

  return ok(expr);
}

void ResultChecker::addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call == nullptr) {
    return;
  }
  Builtin builtin = call->builtin();
  if (builtin != Builtin::ResultIsOk && builtin != Builtin::ResultIsErr) {
    return;
  }
  const VarSymbol *tested = this->testedResult(call);
  if (tested == nullptr) {
    return;
  }
  if (builtin == Builtin::ResultIsOk) {
    return this->splitResultFlows(tested, whenTrue, whenFalse);
  }

  return this->splitResultFlows(tested, whenFalse, whenTrue);
}

const VarSymbol *ResultChecker::testedResult(const CallExpr *test) const {
  const Expr *receiver = cast<MemberExpr>(test->callee)->object;
  const auto *bare = ExprParts::withoutImplicitCasts(receiver);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->var() == nullptr || name->var()->type == nullptr) {
    return nullptr;
  }
  if (!name->var()->type->nonOptional()->isResult()) {
    return nullptr;
  }

  return name->var();
}

void ResultChecker::splitResultFlows(const VarSymbol *tested, Flow &whenOk, Flow &whenErr) const {
  whenOk.okResults.insert(tested);
  whenOk.errResults.erase(tested);
  whenErr.errResults.insert(tested);
  whenErr.okResults.erase(tested);
}

}
