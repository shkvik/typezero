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
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->typeResolver = &all.getTypeResolver();
  this->resultChecker = &all.getResultChecker();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
}

const Type *PromiseStaticChecker::awaited(const Type *type) {
  return type->isPromise() ? type->promised() : type;
}

PromiseStaticChecker::ResultEmptyListTyped
PromiseStaticChecker::emptyList(const Type *wantedElement, llvm::StringRef fn) {
  if (!wantedElement) {
    std::string message = ErrorTextPromiseList::noType(fn);

    return err(PromiseStaticCheckerEmptyListError{
        .code = PromiseStaticCheckerEmptyListErrorCode::ELEMENT_TYPE_UNKNOWN,
        .message = message,
    });
  }

  return ok(wantedElement);
}

PromiseStaticChecker::ResultValueIsNotVoid PromiseStaticChecker::notVoid(const Type *type,
                                                                         llvm::StringRef fn) {
  if (type->isVoid()) {
    std::string message = ErrorTextPromiseVoid::given(fn);

    return err(PromiseStaticCheckerNotVoidError{
        .code = PromiseStaticCheckerNotVoidErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

PromiseStaticChecker::ResultPlainValueType PromiseStaticChecker::plainValue(const Type *type,
                                                                            llvm::StringRef fn) {
  bool mayBePromise = !type->isPromise() && AsyncChecker::awaitedType(type) != type;
  if (mayBePromise) {
    std::string message = ErrorTextPromiseList::mayBe(fn, type->name());

    return err(PromiseStaticCheckerPlainValueError{
        .code = PromiseStaticCheckerPlainValueErrorCode::MAY_BE_PROMISE,
        .message = message,
    });
  }

  return ok(type);
}

PromiseStaticChecker::ResultListOfPromises PromiseStaticChecker::promiseArray(const Expr *arg,
                                                                              llvm::StringRef fn) {
  const Type *type = arg->type;
  if (!type->isArray() && !type->isTuple()) {
    const auto *name = dyn_cast<NameExpr>(arg);
    bool iterable = type->isIterator() || type->isDict();
    llvm::StringRef values = name ? name->name : llvm::StringRef("values");
    std::string message = ErrorTextPromiseList::notArray(fn, type->name(), iterable, values);

    return err(PromiseStaticCheckerPromiseArrayError{
        .code = PromiseStaticCheckerPromiseArrayErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

bool PromiseStaticChecker::checkPromiseList(Expr *&arg, const Type *wantedElement,
                                            const Type *&element, std::vector<const Type *> &tuple,
                                            llvm::StringRef fn) {
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  bool plainLiteral =
      lit != nullptr && llvm::none_of(lit->elements, PromiseStaticChecker::isSpreadOrHole);
  if (plainLiteral) {
    return this->checkLiteralPromiseList(lit, wantedElement, element, tuple, fn);
  }

  return this->checkValuePromiseList(arg, element, tuple, fn);
}

bool PromiseStaticChecker::isSpreadOrHole(const ArrayLitExpr::Element &element) {
  return element.spread || element.hole;
}

bool PromiseStaticChecker::checkLiteralPromiseList(ArrayLitExpr *lit, const Type *wantedElement,
                                                   const Type *&element,
                                                   std::vector<const Type *> &tuple,
                                                   llvm::StringRef fn) {
  if (lit->elements.empty()) {
    ResultEmptyListTyped typed = PromiseStaticChecker::emptyList(wantedElement, fn);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(lit->loc(), typed.error().message);
      return false;
    }
    lit->type = Type::array(Type::promise(typed.value()));
    element = typed.value();
    return true;
  }
  std::vector<const Type *> types;
  if (!this->literalPromiseTypes(lit, wantedElement, fn, types)) {
    return false;
  }
  auto sameAsFirst = [&](const Type *type) { return type == types.front(); };
  bool same = llvm::all_of(types, sameAsFirst);
  const Type *common = PromiseStaticChecker::unifiedType(types);
  if (!same && common != nullptr && !common->isError()) {
    for (ArrayLitExpr::Element &item : lit->elements) {
      item.expr = this->typeRelations->convert(item.expr, common);
    }
    types.assign(types.size(), common);
    same = true;
  }
  if (same) {
    lit->type = Type::array(types.front());
    element = PromiseStaticChecker::awaited(types.front());
    return true;
  }
  lit->type = Type::tuple(types);
  for (const Type *type : types) {
    tuple.push_back(PromiseStaticChecker::awaited(type));
  }

  return true;
}

bool PromiseStaticChecker::literalPromiseTypes(ArrayLitExpr *lit, const Type *wantedElement,
                                               llvm::StringRef fn,
                                               std::vector<const Type *> &types) {
  for (ArrayLitExpr::Element &item : lit->elements) {
    bool shaped = wantedElement != nullptr && Destination::literalShaped(item.expr);
    item.expr = this->exprChecker->checkExpr(item.expr, shaped ? wantedElement : nullptr);
    if (item.expr->type->isError()) {
      return false;
    }
    ResultValueIsNotVoid value = PromiseStaticChecker::notVoid(item.expr->type, fn);
    if (value.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), value.error().message);
      return false;
    }
    ResultPlainValueType plain = PromiseStaticChecker::plainValue(item.expr->type, fn);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(item.expr->loc(), plain.error().message);
      return false;
    }
    types.push_back(item.expr->type);
  }

  return true;
}

const Type *PromiseStaticChecker::unifiedType(const std::vector<const Type *> &types) {
  const Type *common = types.front();
  for (const Type *type : types) {
    if (common != nullptr) {
      common = TypeRelations::unify(common, type);
    }
  }

  return common;
}

bool PromiseStaticChecker::checkValuePromiseList(Expr *&arg, const Type *&element,
                                                 std::vector<const Type *> &tuple,
                                                 llvm::StringRef fn) {
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  ResultListOfPromises list = PromiseStaticChecker::promiseArray(arg, fn);
  if (list.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), list.error().message);
    return false;
  }
  if (type->isArray()) {
    ResultPlainValueType plain = PromiseStaticChecker::plainValue(type->inner(), fn);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return false;
    }
    element = PromiseStaticChecker::awaited(type->inner());
    return true;
  }
  for (const Field &field : type->fields()) {
    ResultPlainValueType plain = PromiseStaticChecker::plainValue(field.type, fn);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return false;
    }
    tuple.push_back(PromiseStaticChecker::awaited(field.type));
  }

  return true;
}

PromiseStaticChecker::ResultTypeArgsAbsent
PromiseStaticChecker::staticTypeArgs(const MemberExpr *member) {
  if (!member->typeArgs.empty() && member->name != "withResolvers") {
    std::string message = ErrorTextStaticTypeArgs::resolvers();

    return err(PromiseStaticCheckerStaticTypeArgsError{
        .code = PromiseStaticCheckerStaticTypeArgsErrorCode::TYPE_ARGUMENTS_UNUSED,
        .message = message,
    });
  }

  return ok(member);
}

PromiseStaticChecker::ResultOneTypeArgOnly
PromiseStaticChecker::resolverTypeArgs(const MemberExpr *member) {
  if (member->typeArgs.size() > 1) {
    std::string message = ErrorTextResolverValue::typeArgs();

    return err(PromiseStaticCheckerResolverTypeArgsError{
        .code = PromiseStaticCheckerResolverTypeArgsErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(member);
}

PromiseStaticChecker::ResultValueTypeKnown PromiseStaticChecker::resolverValue(const Type *value) {
  if (!value) {
    std::string message = ErrorTextResolverValue::unknown();

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
PromiseStaticChecker::raceJoin(const Type *element, const Type *type, llvm::StringRef fn) {
  const Type *common = LiteralTypes::unionUnify(element, type);
  if (!common || common->isError()) {
    std::string message = ErrorTextRaceJoin::differ(fn, element->name(), type->name());

    return err(PromiseStaticCheckerRaceJoinError{
        .code = PromiseStaticCheckerRaceJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

PromiseStaticChecker::ResultTupleHasValues
PromiseStaticChecker::allValues(llvm::ArrayRef<const Type *> tuple) {
  bool hasVoid = llvm::any_of(tuple, [](const Type *t) { return t->isVoid(); });
  if (hasVoid) {
    std::string message = ErrorTextAllValues::voidValue();

    return err(PromiseStaticCheckerAllValuesError{
        .code = PromiseStaticCheckerAllValuesErrorCode::VOID_PROMISE,
        .message = message,
    });
  }
  const Type *values = Type::tuple(tuple);

  return ok(values);
}

Expr *PromiseStaticChecker::checkPromiseStatic(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  ResultTypeArgsAbsent plain = PromiseStaticChecker::staticTypeArgs(member);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    member->typeArgs.clear();
  }
  const Type *wanted = PromiseStaticChecker::wantedValue(expected);
  std::string fn = "Promise." + member->name.str();
  std::optional<PromiseStatic> method = PromiseStaticTable::find(member->name);
  if (!method.has_value()) {
    std::string message = ErrorTextPromiseStatic::unknown(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->checkArgsLoosely(call);
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

const Type *PromiseStaticChecker::wantedValue(const Type *expected) {
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
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
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
  ResultValueIsNotVoid value = PromiseStaticChecker::notVoid(type, fn);
  if (value.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), value.error().message);
    return call;
  }
  call->type = Type::promise(type);

  return call;
}

Expr *PromiseStaticChecker::checkWithResolvers(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseWithResolvers};
  ResultOneTypeArgOnly single = PromiseStaticChecker::resolverTypeArgs(member);
  if (single.isErr()) {
    this->state.getDiagnostics().error(member->loc(), single.error().message);
    return call;
  }
  const Type *value = this->resolverValueType(member, expected);
  member->typeArgs.clear();
  ResultValueTypeKnown known = PromiseStaticChecker::resolverValue(value);
  bool shown = known.isErr() &&
               known.error().code != PromiseStaticCheckerResolverValueErrorCode::ALREADY_REPORTED;
  if (shown) {
    this->state.getDiagnostics().error(member->loc(), known.error().message);
  }
  if (known.isErr()) {
    return call;
  }
  call->type = Type::object({{"promise", Type::promise(value), false},
                             {"resolve", PromiseChecker::resolverType(value), false}});

  return call;
}

const Type *PromiseStaticChecker::resolverValueType(const MemberExpr *member,
                                                    const Type *expected) {
  if (member->typeArgs.size() == 1) {
    const Type *written = this->typeResolver->resolveType(member->typeArgs[0], true);
    return PromiseStaticChecker::awaited(written);
  }
  if (expected == nullptr || !expected->isObject()) {
    return nullptr;
  }
  const Field *promise = expected->field("promise");
  if (promise == nullptr || !promise->type->isPromise()) {
    return nullptr;
  }

  return promise->type->promised();
}

Expr *PromiseStaticChecker::checkCombinator(CallExpr *call, MemberExpr *member, const Type *wanted,
                                            const std::string &fn, Builtin builtin) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  bool race = builtin == Builtin::PromiseRace;
  const Type *wantedElement = nullptr;
  if (wanted != nullptr && !race && wanted->isArray()) {
    wantedElement = wanted->inner();
  } else if (wanted != nullptr && race) {
    wantedElement = wanted;
  }
  const Type *element = nullptr;
  std::vector<const Type *> tuple;
  if (!this->checkPromiseList(call->args[0], wantedElement, element, tuple, fn)) {
    return call;
  }
  if (!tuple.empty() && race) {
    element = tuple.front();
    for (const Type *part : tuple) {
      ResultRaceTypeJoined joined = PromiseStaticChecker::raceJoin(element, part, fn);
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
  if (race) {
    call->type = Type::promise(element);
    return call;
  }

  return this->typeAllCall(call, element, tuple);
}

Expr *PromiseStaticChecker::typeAllCall(CallExpr *call, const Type *element,
                                        const std::vector<const Type *> &tuple) {
  if (!tuple.empty()) {
    ResultTupleHasValues values = PromiseStaticChecker::allValues(tuple);
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
