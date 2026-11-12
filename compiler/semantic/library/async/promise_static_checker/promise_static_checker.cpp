#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker.h"
#include "compiler/program/library/async/promise_resolvers_field_table/promise_resolvers_field_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
PromiseStaticChecker::checkEmptyList(const Type *wantedElement,
                                     llvm::StringRef functionName) const {
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
PromiseStaticChecker::checkNotVoid(const Type *type, llvm::StringRef functionName) const {
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
PromiseStaticChecker::checkPlainValue(const Type *type, llvm::StringRef functionName) const {
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
PromiseStaticChecker::checkPromiseArray(const Expr *arg, llvm::StringRef functionName) const {
  const Type *type = arg->type;
  if (!type->isArray() && !type->isTuple()) {
    const auto *name = dyn_cast<NameExpr>(arg);
    bool isIterable = type->isIterator() || type->isDict();
    std::string values = name != nullptr ? name->name.str() : ErrorTextPromiseList::valuesLabel();
    auto message = ErrorTextPromiseList::notArray(functionName, type->name(), isIterable, values);

    return err(PromiseStaticCheckerPromiseArrayError{
        .code = PromiseStaticCheckerPromiseArrayErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

std::optional<PromiseStaticChecker::PromiseList>
PromiseStaticChecker::promiseList(Expr *&arg, const Type *wantedElement,
                                  llvm::StringRef functionName) {
  auto *literal = dyn_cast<ArrayLitExpr>(arg);
  auto isSpreadOrHole = std::bind_front(&PromiseStaticChecker::isSpreadOrHole, this);
  bool isPlainLiteral = literal != nullptr && llvm::none_of(literal->elements, isSpreadOrHole);
  if (isPlainLiteral) {
    return this->literalPromiseList(literal, wantedElement, functionName);
  }

  return this->valuePromiseList(arg, functionName);
}

bool PromiseStaticChecker::isSpreadOrHole(const ArrayLitExpr::Element &element) const {
  return element.spread || element.hole;
}

std::optional<PromiseStaticChecker::PromiseList>
PromiseStaticChecker::literalPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                                         llvm::StringRef functionName) {
  if (literal->elements.empty()) {
    return this->emptyPromiseList(literal, wantedElement, functionName);
  }
  auto types = this->literalPromiseTypes(literal, wantedElement, functionName);
  if (!types.has_value()) {
    return std::nullopt;
  }
  const auto *same = this->sameLiteralType(literal, types.value());
  if (same != nullptr) {
    literal->type = Type::array(same);
    return PromiseList{.element = this->awaited(same), .tuple = {}};
  }
  literal->type = Type::tuple(types.value());
  auto tuple = std::vector<const Type *>();
  for (const Type *type : types.value()) {
    tuple.push_back(this->awaited(type));
  }

  return PromiseList{.element = nullptr, .tuple = tuple};
}

std::optional<PromiseStaticChecker::PromiseList>
PromiseStaticChecker::emptyPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                                       llvm::StringRef functionName) {
  auto typed = this->checkEmptyList(wantedElement, functionName);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), typed.error().message);
    return std::nullopt;
  }
  literal->type = Type::array(Type::promise(typed.value()));

  return PromiseList{.element = typed.value(), .tuple = {}};
}

const Type *PromiseStaticChecker::sameLiteralType(ArrayLitExpr *literal,
                                                  llvm::ArrayRef<const Type *> types) {
  bool isSame = llvm::all_equal(types);
  const auto *common = this->unifiedType(types);
  if (isSame) {
    return types.front();
  }
  if (common == nullptr || common->isError()) {
    return nullptr;
  }
  for (ArrayLitExpr::Element &item : literal->elements) {
    item.expr = this->typeRelations->convert(item.expr, common);
  }

  return common;
}

std::optional<std::vector<const Type *>>
PromiseStaticChecker::literalPromiseTypes(ArrayLitExpr *literal, const Type *wantedElement,
                                          llvm::StringRef functionName) {
  auto types = std::vector<const Type *>();
  for (ArrayLitExpr::Element &item : literal->elements) {
    bool isShaped = wantedElement != nullptr && this->destination->isLiteralShaped(item.expr);
    const Type *hint = isShaped ? wantedElement : nullptr;
    item.expr = this->exprChecker->checkExpr(item.expr, hint);
    if (item.expr->type->isError()) {
      return std::nullopt;
    }
    auto value = this->checkNotVoid(item.expr->type, functionName);
    if (value.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), value.error().message);
      return std::nullopt;
    }
    auto plain = this->checkPlainValue(item.expr->type, functionName);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), plain.error().message);
      return std::nullopt;
    }
    types.push_back(item.expr->type);
  }

  return types;
}

const Type *PromiseStaticChecker::unifiedType(llvm::ArrayRef<const Type *> types) const {
  const Type *common = types.front();
  for (const Type *type : types) {
    if (common != nullptr) {
      common = this->typeRelations->unify(common, type);
    }
  }

  return common;
}

std::optional<PromiseStaticChecker::PromiseList>
PromiseStaticChecker::valuePromiseList(Expr *&arg, llvm::StringRef functionName) {
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return std::nullopt;
  }
  auto list = this->checkPromiseArray(arg, functionName);
  if (list.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), list.error().message);
    return std::nullopt;
  }
  if (!type->isArray()) {
    return this->tuplePromiseList(arg, functionName);
  }
  auto plain = this->checkPlainValue(type->inner(), functionName);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), plain.error().message);
    return std::nullopt;
  }

  return PromiseList{.element = this->awaited(type->inner()), .tuple = {}};
}

std::optional<PromiseStaticChecker::PromiseList>
PromiseStaticChecker::tuplePromiseList(const Expr *arg, llvm::StringRef functionName) {
  auto tuple = std::vector<const Type *>();
  for (const Field &field : arg->type->fields()) {
    auto plain = this->checkPlainValue(field.type, functionName);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return std::nullopt;
    }
    tuple.push_back(this->awaited(field.type));
  }

  return PromiseList{.element = nullptr, .tuple = tuple};
}

PromiseStaticChecker::ResultTypeArgsAbsent
PromiseStaticChecker::checkStaticTypeArgs(const MemberExpr *member) const {
  auto method = PromiseStaticTable::find(member->name);
  if (!member->typeArgs.empty() && method != PromiseStatic::WITH_RESOLVERS) {
    auto message = ErrorTextStaticTypeArgs::resolvers();

    return err(PromiseStaticCheckerStaticTypeArgsError{
        .code = PromiseStaticCheckerStaticTypeArgsErrorCode::TYPE_ARGUMENTS_UNUSED,
        .message = message,
    });
  }

  return ok(member);
}

PromiseStaticChecker::ResultOneTypeArgOnly
PromiseStaticChecker::checkResolverTypeArgs(const MemberExpr *member) const {
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
PromiseStaticChecker::checkResolverValue(const Type *value) const {
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
PromiseStaticChecker::checkRaceJoin(const Type *element, const Type *type,
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
PromiseStaticChecker::checkAllValues(llvm::ArrayRef<const Type *> tuple) const {
  auto isVoid = std::bind_front(&PromiseStaticChecker::isVoidValue, this);
  if (llvm::any_of(tuple, isVoid)) {
    auto message = ErrorTextAllValues::voidValue();

    return err(PromiseStaticCheckerAllValuesError{
        .code = PromiseStaticCheckerAllValuesErrorCode::VOID_PROMISE,
        .message = message,
    });
  }
  const auto *values = Type::tuple(tuple);

  return ok(values);
}

bool PromiseStaticChecker::isVoidValue(const Type *type) const {
  return type->isVoid();
}

Expr *PromiseStaticChecker::checkPromiseStatic(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto plain = this->checkStaticTypeArgs(member);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    member->typeArgs.clear();
  }
  const auto *wanted = this->wantedValue(expected);
  auto promiseName = GlobalNameTable::getName(GlobalName::PROMISE);
  std::string functionName = promiseName.str() + "." + member->name.str();
  auto method = PromiseStaticTable::find(member->name);
  if (!method.has_value()) {
    auto message = ErrorTextPromiseStatic::unknown(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }

  switch (*method) {
    case PromiseStatic::RESOLVE:
      return this->checkResolve(call, member, wanted, functionName);
    case PromiseStatic::REJECT:
      return this->reportNoReject(call, member, ErrorTextNoReject::rejected());
    case PromiseStatic::ALL_SETTLED:
      return this->reportNoReject(call, member, ErrorTextNoReject::settled());
    case PromiseStatic::ANY:
      return this->reportNoReject(call, member, ErrorTextNoReject::any());
    case PromiseStatic::WITH_RESOLVERS:
      return this->checkWithResolvers(call, member, expected);
    case PromiseStatic::ALL:
      return this->checkCombinator(call, member, wanted, functionName, Builtin::PromiseAll);
    case PromiseStatic::RACE:
      return this->checkCombinator(call, member, wanted, functionName, Builtin::PromiseRace);
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
                                         const std::string &functionName) {
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
  auto value = this->checkNotVoid(type, functionName);
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
  auto single = this->checkResolverTypeArgs(member);
  if (single.isErr()) {
    this->state.getDiagnostics().error(member->loc(), single.error().message);
    return call;
  }
  const auto *value = this->resolverValueType(member, expected);
  member->typeArgs.clear();
  auto known = this->checkResolverValue(value);
  bool isShown = known.isErr() &&
                 known.error().code != PromiseStaticCheckerResolverValueErrorCode::ALREADY_REPORTED;
  if (isShown) {
    this->state.getDiagnostics().error(member->loc(), known.error().message);
  }
  if (known.isErr()) {
    return call;
  }
  auto promiseName = PromiseResolversFieldTable::getName(PromiseResolversField::PROMISE);
  auto resolveName = PromiseResolversFieldTable::getName(PromiseResolversField::RESOLVE);
  const auto *resolver = this->promiseChecker->resolverType(value);
  call->type = Type::object(
      {{promiseName.str(), Type::promise(value), false}, {resolveName.str(), resolver, false}});

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
  auto promiseName = PromiseResolversFieldTable::getName(PromiseResolversField::PROMISE);
  const auto *promise = expected->field(promiseName);
  if (promise == nullptr || !promise->type->isPromise()) {
    return nullptr;
  }

  return promise->type->promised();
}

Expr *PromiseStaticChecker::checkCombinator(CallExpr *call, MemberExpr *member, const Type *wanted,
                                            const std::string &functionName, Builtin builtin) {
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  bool isRace = builtin == Builtin::PromiseRace;
  const auto *wantedElement = this->combinatorElement(wanted, isRace);
  auto list = this->promiseList(call->args[0], wantedElement, functionName);
  if (!list.has_value()) {
    return call;
  }
  if (isRace) {
    return this->typeRaceCall(call, list.value(), wantedElement, functionName);
  }
  const auto *element = this->settledElement(list->element, wantedElement);

  return this->typeAllCall(call, element, list->tuple);
}

const Type *PromiseStaticChecker::combinatorElement(const Type *wanted, bool isRace) const {
  if (wanted != nullptr && !isRace && wanted->isArray()) {
    return wanted->inner();
  }
  if (wanted != nullptr && isRace) {
    return wanted;
  }

  return nullptr;
}

const Type *PromiseStaticChecker::settledElement(const Type *element,
                                                 const Type *wantedElement) const {
  if (element == nullptr || !element->isNever()) {
    return element;
  }

  return wantedElement != nullptr ? wantedElement : Type::voidTy();
}

Expr *PromiseStaticChecker::typeRaceCall(CallExpr *call, const PromiseList &list,
                                         const Type *wantedElement, llvm::StringRef functionName) {
  auto joined = this->checkRaceElement(list, functionName);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), joined.error().message);
    return call;
  }
  const auto *element = this->settledElement(joined.value(), wantedElement);
  call->type = Type::promise(element);

  return call;
}

PromiseStaticChecker::ResultRaceTypeJoined
PromiseStaticChecker::checkRaceElement(const PromiseList &list,
                                       llvm::StringRef functionName) const {
  if (list.tuple.empty()) {
    return ok(list.element);
  }
  const Type *element = list.tuple.front();
  for (const Type *part : list.tuple) {
    auto joined = this->checkRaceJoin(element, part, functionName);
    if (joined.isErr()) {
      return err(joined.error());
    }
    element = joined.value();
  }

  return ok(element);
}

Expr *PromiseStaticChecker::typeAllCall(CallExpr *call, const Type *element,
                                        const std::vector<const Type *> &tuple) {
  if (!tuple.empty()) {
    auto values = this->checkAllValues(tuple);
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
