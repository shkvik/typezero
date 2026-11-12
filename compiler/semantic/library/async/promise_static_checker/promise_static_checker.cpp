#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

PromiseStaticChecker::PromiseStaticChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->destination = &all.destination;
  this->typeResolver = &all.typeResolver;
  this->resultChecker = &all.resultChecker;
  this->typeRelations = &all.typeRelations;
  this->promiseChecker = &all.promiseChecker;
  this->argumentChecker = &all.argumentChecker;
}

const Type *PromiseStaticChecker::awaited(const Type *type) const {
  return type->isPromise() ? type->promised() : type;
}

PromiseStaticChecker::ResultEmptyListTyped
PromiseStaticChecker::emptyList(const Type *wantedElement, llvm::StringRef functionName) const {
  if (wantedElement == nullptr) {
    auto message = ErrorTextPromiseList::noType(functionName);

    return err(PromiseStaticCheckerEmptyListError{
        .code = PromiseStaticCheckerEmptyListErrorCode::ELEMENT_TYPE_UNKNOWN,
        .message = message,
    });
  }

  return ok(wantedElement);
}

PromiseStaticChecker::ResultValueIsNotVoid
PromiseStaticChecker::notVoid(const Type *type, llvm::StringRef functionName) const {
  if (type->isVoid()) {
    auto message = ErrorTextPromiseVoid::given(functionName);

    return err(PromiseStaticCheckerNotVoidError{
        .code = PromiseStaticCheckerNotVoidErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

PromiseStaticChecker::ResultPlainValueType
PromiseStaticChecker::plainValue(const Type *type, llvm::StringRef functionName) const {
  bool isMayBePromise = !type->isPromise() && AsyncChecker::awaitedType(type) != type;
  if (isMayBePromise) {
    auto message = ErrorTextPromiseList::mayBe(functionName, type->name());

    return err(PromiseStaticCheckerPlainValueError{
        .code = PromiseStaticCheckerPlainValueErrorCode::MAY_BE_PROMISE,
        .message = message,
    });
  }

  return ok(type);
}

PromiseStaticChecker::ResultListOfPromises
PromiseStaticChecker::promiseArray(const Expr *arg, llvm::StringRef functionName) const {
  const Type *type = arg->type;
  if (!type->isArray() && !type->isTuple()) {
    const auto *name = dyn_cast<NameExpr>(arg);
    bool isIterable = type->isIterator() || type->isDict();
    llvm::StringRef values = name != nullptr ? name->name : llvm::StringRef("values");
    auto message = ErrorTextPromiseList::notArray(functionName, type->name(), isIterable, values);

    return err(PromiseStaticCheckerPromiseArrayError{
        .code = PromiseStaticCheckerPromiseArrayErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

bool PromiseStaticChecker::checkPromiseList(Expr *&arg, const Type *wantedElement,
                                            const Type *&element, std::vector<const Type *> &tuple,
                                            llvm::StringRef functionName) {
  auto *literal = dyn_cast<ArrayLitExpr>(arg);
  bool isPlainLiteral =
      literal != nullptr && llvm::none_of(literal->elements, PromiseStaticChecker::isSpreadOrHole);
  if (isPlainLiteral) {
    return this->checkLiteralPromiseList(literal, wantedElement, element, tuple, functionName);
  }

  return this->checkValuePromiseList(arg, element, tuple, functionName);
}

bool PromiseStaticChecker::isSpreadOrHole(const ArrayLitExpr::Element &element) {
  return element.spread || element.hole;
}

bool PromiseStaticChecker::checkLiteralPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                                                   const Type *&element,
                                                   std::vector<const Type *> &tuple,
                                                   llvm::StringRef functionName) {
  if (literal->elements.empty()) {
    auto typed = this->emptyList(wantedElement, functionName);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(literal->loc(), typed.error().message);
      return false;
    }
    literal->type = Type::array(Type::promise(typed.value()));
    element = typed.value();
    return true;
  }
  auto types = std::vector<const Type *>();
  if (!this->literalPromiseTypes(literal, wantedElement, functionName, types)) {
    return false;
  }
  auto sameAsFirst = [&](const Type *type) { return type == types.front(); };
  bool isSame = llvm::all_of(types, sameAsFirst);
  const auto *common = this->unifiedType(types);
  if (!isSame && common != nullptr && !common->isError()) {
    for (ArrayLitExpr::Element &item : literal->elements) {
      item.expr = this->typeRelations->convert(item.expr, common);
    }
    types.assign(types.size(), common);
    isSame = true;
  }
  if (isSame) {
    literal->type = Type::array(types.front());
    element = this->awaited(types.front());
    return true;
  }
  literal->type = Type::tuple(types);
  for (const Type *type : types) {
    tuple.push_back(this->awaited(type));
  }

  return true;
}

bool PromiseStaticChecker::literalPromiseTypes(ArrayLitExpr *literal, const Type *wantedElement,
                                               llvm::StringRef functionName,
                                               std::vector<const Type *> &types) {
  for (ArrayLitExpr::Element &item : literal->elements) {
    bool isShaped = wantedElement != nullptr && this->destination->isLiteralShaped(item.expr);
    item.expr = this->exprChecker->checkExpr(item.expr, isShaped ? wantedElement : nullptr);
    if (item.expr->type->isError()) {
      return false;
    }
    auto value = this->notVoid(item.expr->type, functionName);
    if (value.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), value.error().message);
      return false;
    }
    auto plain = this->plainValue(item.expr->type, functionName);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), plain.error().message);
      return false;
    }
    types.push_back(item.expr->type);
  }

  return true;
}

const Type *PromiseStaticChecker::unifiedType(const std::vector<const Type *> &types) const {
  const Type *common = types.front();
  for (const Type *type : types) {
    if (common != nullptr) {
      common = this->typeRelations->unify(common, type);
    }
  }

  return common;
}

bool PromiseStaticChecker::checkValuePromiseList(Expr *&arg, const Type *&element,
                                                 std::vector<const Type *> &tuple,
                                                 llvm::StringRef functionName) {
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  auto list = this->promiseArray(arg, functionName);
  if (list.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), list.error().message);
    return false;
  }
  if (type->isArray()) {
    auto plain = this->plainValue(type->inner(), functionName);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return false;
    }
    element = this->awaited(type->inner());
    return true;
  }
  for (const Field &field : type->fields()) {
    auto plain = this->plainValue(field.type, functionName);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return false;
    }
    tuple.push_back(this->awaited(field.type));
  }

  return true;
}

PromiseStaticChecker::ResultTypeArgsAbsent
PromiseStaticChecker::staticTypeArgs(const MemberExpr *member) const {
  if (!member->typeArgs.empty() && member->name != "withResolvers") {
    auto message = ErrorTextStaticTypeArgs::resolvers();

    return err(PromiseStaticCheckerStaticTypeArgsError{
        .code = PromiseStaticCheckerStaticTypeArgsErrorCode::TYPE_ARGUMENTS_UNUSED,
        .message = message,
    });
  }

  return ok(member);
}

PromiseStaticChecker::ResultOneTypeArgOnly
PromiseStaticChecker::resolverTypeArgs(const MemberExpr *member) const {
  if (member->typeArgs.size() > 1) {
    auto message = ErrorTextResolverValue::typeArgs();

    return err(PromiseStaticCheckerResolverTypeArgsError{
        .code = PromiseStaticCheckerResolverTypeArgsErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(member);
}

PromiseStaticChecker::ResultValueTypeKnown
PromiseStaticChecker::resolverValue(const Type *value) const {
  if (value == nullptr) {
    auto message = ErrorTextResolverValue::unknown();

    return err(PromiseStaticCheckerResolverValueError{
        .code = PromiseStaticCheckerResolverValueErrorCode::VALUE_TYPE_UNKNOWN,
        .message = message,
    });
  }
  if (value->isError()) {
    return err(PromiseStaticCheckerResolverValueError{
        .code = PromiseStaticCheckerResolverValueErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return ok(value);
}

PromiseStaticChecker::ResultRaceTypeJoined
PromiseStaticChecker::raceJoin(const Type *element, const Type *type,
                               llvm::StringRef functionName) const {
  const auto *common = LiteralTypes::unionUnify(element, type);
  if (common == nullptr || common->isError()) {
    auto message = ErrorTextRaceJoin::differ(functionName, element->name(), type->name());

    return err(PromiseStaticCheckerRaceJoinError{
        .code = PromiseStaticCheckerRaceJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

PromiseStaticChecker::ResultTupleHasValues
PromiseStaticChecker::allValues(llvm::ArrayRef<const Type *> tuple) const {
  bool hasVoid = llvm::any_of(tuple, [](const Type *type) { return type->isVoid(); });
  if (hasVoid) {
    auto message = ErrorTextAllValues::voidValue();

    return err(PromiseStaticCheckerAllValuesError{
        .code = PromiseStaticCheckerAllValuesErrorCode::VOID_PROMISE,
        .message = message,
    });
  }
  const auto *values = Type::tuple(tuple);

  return ok(values);
}

Expr *PromiseStaticChecker::checkPromiseStatic(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto plain = this->staticTypeArgs(member);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    member->typeArgs.clear();
  }
  const auto *wanted = this->wantedValue(expected);
  std::string fn = "Promise." + member->name.str();
  auto method = PromiseStaticTable::find(member->name);
  if (!method.has_value()) {
    auto message = ErrorTextPromiseStatic::unknown(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }

  switch (*method) {
    case PromiseStatic::RESOLVE:
      return this->checkResolve(call, member, wanted, fn);
    case PromiseStatic::REJECT:
      return this->reportNoReject(call, member, ErrorTextNoReject::rejected());
    case PromiseStatic::ALL_SETTLED:
      return this->reportNoReject(call, member, ErrorTextNoReject::settled());
    case PromiseStatic::ANY:
      return this->reportNoReject(call, member, ErrorTextNoReject::any());
    case PromiseStatic::WITH_RESOLVERS:
      return this->checkWithResolvers(call, member, expected);
    case PromiseStatic::ALL:
      return this->checkCombinator(call, member, wanted, fn, Builtin::PromiseAll);
    case PromiseStatic::RACE:
      return this->checkCombinator(call, member, wanted, fn, Builtin::PromiseRace);
  }

  return call;
}

const Type *PromiseStaticChecker::wantedValue(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }
  const Type *wanted = expected->nonOptional()->promised();

  return wanted->isNever() ? nullptr : wanted;
}

Expr *PromiseStaticChecker::reportNoReject(CallExpr *call, MemberExpr *member,
                                           const std::string &message) {
  this->state.getDiagnostics().error(member->loc(), message);

  return this->resultChecker->markCallFailed(call);
}

Expr *PromiseStaticChecker::checkResolve(CallExpr *call, MemberExpr *member, const Type *wanted,
                                         const std::string &fn) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseResolve};
  if (call->args.empty()) {
    call->type = Type::promise(Type::voidTy());
    return call;
  }
  Expr *&arg = call->args[0];
  const Type *hint = wanted != nullptr && !isa<UndefinedExpr>(arg) ? wanted : nullptr;
  arg = this->exprChecker->checkExpr(arg, hint);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (type->isPromise()) {
    call->type = type;
    return call;
  }
  if (type->isUndefined()) {
    call->type = Type::promise(Type::voidTy());
    return call;
  }
  auto value = this->notVoid(type, fn);
  if (value.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), value.error().message);
    return call;
  }
  call->type = Type::promise(type);

  return call;
}

Expr *PromiseStaticChecker::checkWithResolvers(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseWithResolvers};
  auto single = this->resolverTypeArgs(member);
  if (single.isErr()) {
    this->state.getDiagnostics().error(member->loc(), single.error().message);
    return call;
  }
  const auto *value = this->resolverValueType(member, expected);
  member->typeArgs.clear();
  auto known = this->resolverValue(value);
  bool isShown = known.isErr() &&
                 known.error().code != PromiseStaticCheckerResolverValueErrorCode::ALREADY_REPORTED;
  if (isShown) {
    this->state.getDiagnostics().error(member->loc(), known.error().message);
  }
  if (known.isErr()) {
    return call;
  }
  call->type = Type::object({{"promise", Type::promise(value), false},
                             {"resolve", this->promiseChecker->resolverType(value), false}});

  return call;
}

const Type *PromiseStaticChecker::resolverValueType(const MemberExpr *member,
                                                    const Type *expected) {
  if (member->typeArgs.size() == 1) {
    const auto *written = this->typeResolver->resolveTypeOrVoid(member->typeArgs[0]);
    return this->awaited(written);
  }
  if (expected == nullptr || !expected->isObject()) {
    return nullptr;
  }
  const auto *promise = expected->field("promise");
  if (promise == nullptr || !promise->type->isPromise()) {
    return nullptr;
  }

  return promise->type->promised();
}

Expr *PromiseStaticChecker::checkCombinator(CallExpr *call, MemberExpr *member, const Type *wanted,
                                            const std::string &fn, Builtin builtin) {
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  bool isRace = builtin == Builtin::PromiseRace;
  const Type *wantedElement = nullptr;
  if (wanted != nullptr && !isRace && wanted->isArray()) {
    wantedElement = wanted->inner();
  } else if (wanted != nullptr && isRace) {
    wantedElement = wanted;
  }
  const Type *element = nullptr;
  auto tuple = std::vector<const Type *>();
  if (!this->checkPromiseList(call->args[0], wantedElement, element, tuple, fn)) {
    return call;
  }
  if (!tuple.empty() && isRace) {
    element = tuple.front();
    for (const Type *part : tuple) {
      auto joined = this->raceJoin(element, part, fn);
      if (joined.isErr()) {
        this->state.getDiagnostics().error(call->args[0]->loc(), joined.error().message);
        return call;
      }
      element = joined.value();
    }
  }
  if (element != nullptr && element->isNever()) {
    element = wantedElement != nullptr ? wantedElement : Type::voidTy();
  }
  if (isRace) {
    call->type = Type::promise(element);
    return call;
  }

  return this->typeAllCall(call, element, tuple);
}

Expr *PromiseStaticChecker::typeAllCall(CallExpr *call, const Type *element,
                                        const std::vector<const Type *> &tuple) {
  if (!tuple.empty()) {
    auto values = this->allValues(tuple);
    if (values.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), values.error().message);
      return call;
    }
    call->type = Type::promise(values.value());
    return call;
  }
  if (element->isVoid()) {
    call->type = Type::promise(Type::voidTy());
    return call;
  }
  call->type = Type::promise(Type::array(element));

  return call;
}

}
