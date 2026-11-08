#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *PromiseStaticChecker::awaited(const Type *type) {
  return type->isPromise() ? type->promised() : type;
}

PromiseStaticChecker::PromiseStaticChecker(SemanticState &owner)
    : state(owner) {}

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
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && llvm::none_of(lit->elements,
                           [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
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
    bool same = true;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr =
          ExprChecker(this->state)
              .checkExpr(e.expr, wantedElement && Destination::literalShaped(e.expr) ? wantedElement
                                                                                     : nullptr);
      if (e.expr->type->isError()) {
        return false;
      }
      ResultValueIsNotVoid value = PromiseStaticChecker::notVoid(e.expr->type, fn);
      if (value.isErr()) {
        this->state.getDiagnostics().error(e.expr->loc(), value.error().message);
        return false;
      }
      ResultPlainValueType plain = PromiseStaticChecker::plainValue(e.expr->type, fn);
      if (plain.isErr()) {
        this->state.getDiagnostics().error(e.expr->loc(), plain.error().message);
        return false;
      }
      if (!types.empty() && types.front() != e.expr->type) {
        same = false;
      }
      types.push_back(e.expr->type);
    }
    const Type *common = types.front();
    for (const Type *t : types) {
      if (common) {
        common = TypeRelations::unify(common, t);
      }
    }
    if (!same && common && !common->isError()) {
      for (ArrayLitExpr::Element &e : lit->elements) {
        e.expr = TypeRelations(this->state).convert(e.expr, common);
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
    for (const Type *t : types) {
      tuple.push_back(PromiseStaticChecker::awaited(t));
    }
    return true;
  }
  arg = ExprChecker(this->state).checkExpr(arg);
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
  for (const Field &f : type->fields()) {
    ResultPlainValueType plain = PromiseStaticChecker::plainValue(f.type, fn);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), plain.error().message);
      return false;
    }
    tuple.push_back(PromiseStaticChecker::awaited(f.type));
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
  llvm::StringRef name = member->name;
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  ResultTypeArgsAbsent plain = PromiseStaticChecker::staticTypeArgs(member);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    member->typeArgs.clear();
  }
  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  if (wanted && wanted->isNever()) {
    wanted = nullptr;
  }
  std::string fn = "Promise." + name.str();

  if (name == "resolve") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
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
    arg = ExprChecker(this->state)
              .checkExpr(arg, wanted && !isa<UndefinedExpr>(arg) ? wanted : nullptr);
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
  if (name == "reject" || name == "allSettled" || name == "any") {
    std::string message = name == "reject"       ? ErrorTextNoReject::rejected()
                          : name == "allSettled" ? ErrorTextNoReject::settled()
                                                 : ErrorTextNoReject::any();
    this->state.getDiagnostics().error(member->loc(), message);

    return ResultChecker(this->state).markCallFailed(call);
  }
  if (name == "withResolvers") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
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
    const Type *value = nullptr;
    if (member->typeArgs.size() == 1) {
      value = PromiseStaticChecker::awaited(
          TypeResolver(this->state).resolveType(member->typeArgs[0], true));
    } else if (expected && expected->isObject()) {
      if (const Field *promise = expected->field("promise");
          promise && promise->type->isPromise()) {
        value = promise->type->promised();
      }
    }
    member->typeArgs.clear();
    ResultValueTypeKnown known = PromiseStaticChecker::resolverValue(value);
    if (known.isErr() &&
        known.error().code != PromiseStaticCheckerResolverValueErrorCode::ALREADY_REPORTED) {
      this->state.getDiagnostics().error(member->loc(), known.error().message);
    }
    if (known.isErr()) {
      return call;
    }
    call->type = Type::object({{"promise", Type::promise(value), false},
                               {"resolve", PromiseChecker::resolverType(value), false}});
    return call;
  }
  static const std::pair<llvm::StringLiteral, Builtin> lists[] = {{"all", Builtin::PromiseAll},
                                                                  {"race", Builtin::PromiseRace}};
  for (const auto &[listName, builtin] : lists) {
    if (name != listName) {
      continue;
    }
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = builtin};
    const Type *wantedElement = nullptr;
    if (wanted && builtin == Builtin::PromiseAll && wanted->isArray()) {
      wantedElement = wanted->inner();
    } else if (wanted && builtin == Builtin::PromiseRace) {
      wantedElement = wanted;
    }
    const Type *element = nullptr;
    std::vector<const Type *> tuple;
    if (!this->checkPromiseList(call->args[0], wantedElement, element, tuple, fn)) {
      return call;
    }
    if (!tuple.empty() && builtin == Builtin::PromiseRace) {
      element = tuple.front();
      for (const Type *t : tuple) {
        ResultRaceTypeJoined joined = PromiseStaticChecker::raceJoin(element, t, fn);
        if (joined.isErr()) {
          this->state.getDiagnostics().error(call->args[0]->loc(), joined.error().message);
          return call;
        }
        element = joined.value();
      }
    }
    if (element && element->isNever()) {
      element = wantedElement ? wantedElement : Type::voidTy();
    }
    switch (builtin) {
    case Builtin::PromiseAll:
      if (!tuple.empty()) {
        ResultTupleHasValues values = PromiseStaticChecker::allValues(tuple);
        if (values.isErr()) {
          this->state.getDiagnostics().error(call->args[0]->loc(), values.error().message);
          return call;
        }
        call->type = Type::promise(values.value());
      } else if (element->isVoid()) {
        call->type = Type::promise(Type::voidTy());
      } else {
        call->type = Type::promise(Type::array(element));
      }
      break;
    default:
      call->type = Type::promise(element);
      break;
    }
    return call;
  }
  std::string message = ErrorTextPromiseStatic::unknown(name);
  this->state.getDiagnostics().error(member->loc(), message);
  ArgumentChecker(this->state).checkArgsLoosely(call);

  return call;
}

}
