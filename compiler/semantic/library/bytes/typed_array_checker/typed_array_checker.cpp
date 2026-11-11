#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TypedArrayChecker::TypedArrayChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->byteProofs = &all.byteProofs;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->mathChecker = &all.mathChecker;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->bytesNewChecker = &all.bytesNewChecker;
  this->callbackChecker = &all.callbackChecker;
  this->iteratorMethods = &all.iteratorMethods;
}

TypedArrayChecker::ResultSetSourceOk TypedArrayChecker::setSource(const Expr *source) {
  if (source->type->isIterator()) {
    std::string message = ErrorTextTypedArray::iterator();

    return err(TypedArrayCheckerSetSourceError{
        .code = TypedArrayCheckerSetSourceErrorCode::ITERATOR_SOURCE,
        .message = message,
    });
  }

  return ok(source);
}

Expr *TypedArrayChecker::checkTypedArrayMethod(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  const BytesMethodInfo *found = BytesMethodTable::find(member->name);
  if (found == nullptr) {
    return this->reportMissingMethod(call, member, found);
  }

  TypedArrayCall typed = TypedArrayChecker::describeCall(call, member);
  const BytesMethodForm &form = found->form;
  switch (form.kind()) {
    case BytesMethodKind::TYPED_ARRAY_CALLBACK:
      return this->checkCallbackCall(typed, found->method, expected);
    case BytesMethodKind::TYPED_ARRAY_SORT:
      return this->checkSort(typed, *form.as<TypedArraySortMethod>(), expected);
    case BytesMethodKind::TYPED_ARRAY_REVERSE:
      return this->checkReverse(typed, *form.as<TypedArrayReverseMethod>());
    case BytesMethodKind::TYPED_ARRAY_AT:
      return this->checkAt(typed);
    case BytesMethodKind::TYPED_ARRAY_SEARCH:
      return this->checkSearch(typed, *form.as<TypedArraySearchMethod>());
    case BytesMethodKind::TYPED_ARRAY_JOIN:
      return this->checkJoin(typed, *form.as<TypedArrayJoinMethod>());
    case BytesMethodKind::TYPED_ARRAY_FILL:
      return this->checkFill(typed);
    case BytesMethodKind::TYPED_ARRAY_SET:
      return this->checkSet(typed);
    case BytesMethodKind::TYPED_ARRAY_POSITIONS:
      return this->checkPositions(typed, *form.as<TypedArrayPositionsMethod>());
    case BytesMethodKind::TYPED_ARRAY_WITH:
      return this->checkWith(typed);
    case BytesMethodKind::TYPED_ARRAY_ITERATOR:
      return this->checkIterator(typed);
    case BytesMethodKind::NOT_ON_TYPED_ARRAY:
      return this->reportMissingMethod(call, member, found);
  }

  return call;
}

TypedArrayChecker::TypedArrayCall TypedArrayChecker::describeCall(CallExpr *call,
                                                                  MemberExpr *member) {
  const Type *arrayType = member->object->type;
  const Type *copyType = arrayType->isBuffer() ? Type::bytesType(Bytes::Uint8) : arrayType;
  std::string what = ErrorTextByteView::element(arrayType->name());

  return TypedArrayCall{
      .call = call,
      .member = member,
      .arrayType = arrayType,
      .element = arrayType->elementType(),
      .copyType = copyType,
      .what = what,
  };
}

Expr *TypedArrayChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *TypedArrayChecker::checkCallbackCall(const TypedArrayCall &typed, BytesMethod method,
                                           const Type *expected) {
  CallExpr *call = typed.call;
  bool arrow = !call->args.empty() && isa<FunctionExpr>(call->args[0]);
  Expr *checked = this->callbackChecker->checkCallbackMethod(call, typed.member, expected);
  bool mapsNamedFunction = method == BytesMethod::MAP && !call->args.empty() && !arrow;
  if (mapsNamedFunction) {
    ResultMapFitsType fits = this->mapResultFits(typed);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), fits.error().message);
    }
  }

  return checked;
}

TypedArrayChecker::ResultMapFitsType
TypedArrayChecker::mapResultFits(const TypedArrayCall &typed) const {
  const Expr *callback = typed.call->args[0];
  int64_t lo = 0;
  int64_t hi = 0;
  bool bounded = callback->type->isFunction() && typed.arrayType->elementRange(lo, hi) &&
                 typed.arrayType->bytes() != Bytes::Uint8Clamped;
  if (bounded) {
    std::string range = ErrorTextBytes::range(lo, hi);
    std::string hint = BytesCheckHelpers::wrapHint(typed.arrayType->bytes(), "f(x)");
    std::string message = ErrorTextTypedArray::mapFits(typed.what, range, hint);

    return err(TypedArrayCheckerMapResultFitsError{
        .code = TypedArrayCheckerMapResultFitsErrorCode::RESULT_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(callback);
}

Expr *TypedArrayChecker::checkSort(const TypedArrayCall &typed, const TypedArraySortMethod &sort,
                                   const Type *expected) {
  CallExpr *call = typed.call;
  bool copies = sort.place == ReorderPlace::NEW_COPY;
  if (!call->args.empty()) {
    Expr *checked = this->callbackChecker->checkCallbackMethod(call, typed.member, expected);
    if (copies && !call->type->isError()) {
      call->type = typed.copyType;
    }
    return checked;
  }
  const Type *sorted = copies ? typed.copyType : typed.arrayType;

  return TypedArrayChecker::toBuiltinCall(call, sort.builtin, sorted);
}

Expr *TypedArrayChecker::checkReverse(const TypedArrayCall &typed,
                                      const TypedArrayReverseMethod &reverse) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  bool copies = reverse.place == ReorderPlace::NEW_COPY;
  const Type *reversed = copies ? typed.copyType : typed.arrayType;

  return TypedArrayChecker::toBuiltinCall(call, reverse.builtin, reversed);
}

Expr *TypedArrayChecker::checkAt(const TypedArrayCall &typed) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  this->walkPositionArgs(call, 0);
  bool inRange = this->indexProofs->provesAtInBounds(typed.member->object, call->args[0]);
  const Type *answer = inRange ? typed.element : Type::optional(typed.element);
  call->mayBeOutOfRange = !inRange;

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesAt, answer);
}

Expr *TypedArrayChecker::checkSearch(const TypedArrayCall &typed,
                                     const TypedArraySearchMethod &search) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  Expr *value = this->mathChecker->numericArg(call->args[0], typed.element);
  if (!value->type->isNumeric()) {
    return call;
  }

  int runtimeMode = 0;
  const Type *answer = Type::intTy();
  switch (search.mode) {
    case BytesSearchMode::INDEX_OF:
      break;
    case BytesSearchMode::LAST_INDEX_OF:
      runtimeMode = 1;
      break;
    case BytesSearchMode::INCLUDES:
      runtimeMode = 2;
      answer = Type::boolTy();
      break;
  }

  IntLitExpr *mode = this->makeIntLiteral(call->loc(), runtimeMode);
  Expr *needle = this->typeRelations->convert(value, Type::doubleTy());
  std::vector<Expr *> args{mode, needle};
  if (call->args.size() == 2) {
    Expr *from = this->memberChecker->checkPosition(call->args[1]);
    args.push_back(from);
  }
  call->args = std::move(args);

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesIndexOf, answer);
}

Expr *TypedArrayChecker::checkJoin(const TypedArrayCall &typed, const TypedArrayJoinMethod &join) {
  CallExpr *call = typed.call;
  unsigned maxArgs = static_cast<unsigned>(join.maxArgs);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  }

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesJoin, Type::string());
}

Expr *TypedArrayChecker::checkFill(const TypedArrayCall &typed) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  Expr *value = this->argumentChecker->checkArg(call->args[0], typed.element);
  ByteProofs::ResultValueFitsType fitting =
      this->byteProofs->valueFits(value, typed.arrayType, typed.what);
  if (fitting.isErr()) {
    this->state.getDiagnostics().error(value->loc(), fitting.error().message);
  }
  call->args[0] = this->typeRelations->convert(value, Type::doubleTy());
  this->walkPositionArgs(call, 1);

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesFill, typed.arrayType);
}

Expr *TypedArrayChecker::checkSet(const TypedArrayCall &typed) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }

  Expr *&source = call->args[0];
  auto *literal = dyn_cast<ArrayLitExpr>(source);
  if (literal != nullptr) {
    const Type *literalType = Type::array(typed.element);
    source = this->exprChecker->checkExpr(literal, literalType);
    if (source->type->isError()) {
      return call;
    }
    for (const ArrayLitExpr::Element &item : literal->elements) {
      if (item.spread) {
        if (!this->bytesNewChecker->checkBytesSource(item.expr, typed.arrayType)) {
          return call;
        }
        continue;
      }
      ByteProofs::ResultValueFitsType fitting =
          this->byteProofs->valueFits(item.expr, typed.arrayType, typed.what);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(item.expr->loc(), fitting.error().message);
      }
    }
  } else {
    source = this->exprChecker->checkExpr(source);
    ResultSetSourceOk set = TypedArrayChecker::setSource(source);
    if (set.isErr()) {
      this->state.getDiagnostics().error(source->loc(), set.error().message);
      return call;
    }
    if (!this->bytesNewChecker->checkBytesSource(source, typed.arrayType)) {
      return call;
    }
  }

  this->walkPositionArgs(call, 1);
  const Expr *offset = call->args.size() > 1 ? call->args[1] : nullptr;
  this->checkSetRange(typed.member->object, source, offset);

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesSet, Type::voidTy());
}

Expr *TypedArrayChecker::checkPositions(const TypedArrayCall &typed,
                                        const TypedArrayPositionsMethod &positions) {
  CallExpr *call = typed.call;
  unsigned minArgs = static_cast<unsigned>(positions.minArgs);
  unsigned maxArgs = static_cast<unsigned>(positions.maxArgs);
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  this->walkPositionArgs(call, 0);

  return TypedArrayChecker::toBuiltinCall(call, positions.builtin, typed.arrayType);
}

Expr *TypedArrayChecker::checkWith(const TypedArrayCall &typed) {
  CallExpr *call = typed.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(typed.member->loc(), arity.error().message);
    return call;
  }
  Expr *index = this->memberChecker->checkPosition(call->args[0]);
  call->args[0] = index;
  ResultIndexProven proven = this->withIndex(typed.member->object, index);
  if (proven.isErr()) {
    this->state.getDiagnostics().error(index->loc(), proven.error().message);
  }
  Expr *value = this->argumentChecker->checkArg(call->args[1], typed.element);
  ByteProofs::ResultValueFitsType fitting =
      this->byteProofs->valueFits(value, typed.arrayType, typed.what);
  if (fitting.isErr()) {
    this->state.getDiagnostics().error(value->loc(), fitting.error().message);
  }
  call->args[1] = this->typeRelations->convert(value, Type::doubleTy());

  return TypedArrayChecker::toBuiltinCall(call, Builtin::BytesWith, typed.copyType);
}

TypedArrayChecker::ResultIndexProven TypedArrayChecker::withIndex(const Expr *array,
                                                                  const Expr *index) const {
  bool unproven = !index->type->isError() && !this->indexProofs->provesAtInBounds(array, index) &&
                  !this->indexProofs->exempt(index->loc(), array);
  if (unproven) {
    std::string arrayText = BytesCheckHelpers::valueText(array);
    std::string message = ErrorTextTypedArray::index(arrayText);

    return err(TypedArrayCheckerWithIndexError{
        .code = TypedArrayCheckerWithIndexErrorCode::INDEX_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(index);
}

Expr *TypedArrayChecker::checkIterator(const TypedArrayCall &typed) {
  CallExpr *call = typed.call;
  MemberExpr *member = typed.member;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  CallExpr *copied = this->makeArrayFrom(typed);
  auto *iterated = this->state.getAst().make<MemberExpr>(member->loc(), copied, member->name);
  auto *iterator =
      this->state.getAst().make<CallExpr>(call->loc(), iterated, std::vector<Expr *>{});
  IteratorMethods::ResultArrayIterateCall iterate =
      this->iteratorMethods->checkArrayIterator(iterator, iterated);
  if (iterate.isErr()) {
    this->state.getDiagnostics().error(iterated->loc(), iterate.error().message);
    return iterator;
  }

  return iterate.value();
}

CallExpr *TypedArrayChecker::makeArrayFrom(const TypedArrayCall &typed) {
  MemberExpr *member = typed.member;
  llvm::StringRef arrayGlobal = GlobalNameTable::getName(GlobalName::ARRAY);
  llvm::StringRef fromName = GlobalMemberTable::getName(GlobalMember::ARRAY_FROM);
  auto *arrayName = this->state.getAst().make<NameExpr>(member->loc(), arrayGlobal);
  auto *callee = this->state.getAst().make<MemberExpr>(member->loc(), arrayName, fromName);
  auto *from = this->state.getAst().make<CallExpr>(member->loc(), callee,
                                                   std::vector<Expr *>{member->object});
  from->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  from->type = Type::array(typed.element);

  return from;
}

Expr *TypedArrayChecker::reportMissingMethod(CallExpr *call, MemberExpr *member,
                                             const BytesMethodInfo *found) {
  bool flatMap = found != nullptr && found->method == BytesMethod::FLAT_MAP;
  if (flatMap) {
    std::string message = ErrorTextTypedArray::flatMap();
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->checkArgsLoosely(call);
    return call;
  }

  this->argumentChecker->checkArgsLoosely(call);
  bool locale = found != nullptr && found->method == BytesMethod::TO_LOCALE_STRING;
  std::string message =
      locale ? ErrorTextTypedArray::locale()
             : ErrorTextTypedArray::noMember(member->name, member->object->type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

void TypedArrayChecker::walkPositionArgs(CallExpr *call, size_t from) {
  for (size_t i = from; i < call->args.size(); ++i) {
    call->args[i] = this->memberChecker->checkPosition(call->args[i]);
  }
}

IntLitExpr *TypedArrayChecker::makeIntLiteral(llvm::SMLoc loc, int value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

Expr *TypedArrayChecker::checkDataViewMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  NumberMethod m;
  if (!BytesCheckHelpers::numberMethod(name, true, m)) {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    std::string message = ErrorTextTypedArray::noMember(name, "DataView");
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  if (m.bigint) {
    std::string quoted = ErrorTextBigInt::quoted(name);
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(member->loc(), bigint);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const Type *kindType = Type::bytesType(m.kind);
  const Type *element = kindType->accessType();
  bool oneByte = kindType->elementSize() == 1;
  unsigned min = m.write ? 2 : 1;
  unsigned max = min + (oneByte ? 0 : 1);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *kind = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(m.kind));
  kind->type = Type::intTy();
  std::vector<Expr *> args{kind, this->memberChecker->checkPosition(call->args[0])};
  if (m.write) {
    Expr *value = this->argumentChecker->checkArg(call->args[1], element);
    ByteProofs::ResultValueFitsType fitting =
        this->byteProofs->valueFits(value, kindType, BytesCheckHelpers::numberWhat(m.kind));
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(value->loc(), fitting.error().message);
    }
    args.push_back(value);
  }
  if (call->args.size() > min) {
    args.push_back(this->argumentChecker->checkArg(call->args[min], Type::boolTy()));
  }
  std::string offsetWhat = ErrorTextTypedArray::offsetOf(member->name);
  IndexProofs::ResultUpToWasProven upTo =
      this->indexProofs->requireUpTo(args[1], member->object, kindType->elementSize(), offsetWhat);
  if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(args[1]->loc(), upTo.error().message);
  }
  call->mayBeOutOfRange = upTo.isErr();
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = m.write ? Builtin::DataViewSet : Builtin::DataViewGet};
  call->type = m.write ? Type::voidTy()
               : element->isFloat()
                   ? Type::result(Type::doubleTy(), this->prelude->builtinType("NumberError"))
                   : element;

  return call;
}

void TypedArrayChecker::checkSetRange(const Expr *target, const Expr *source, const Expr *offset) {
  IndexProofs &proofs = *this->indexProofs;
  bool offsetUnproven = false;
  if (offset) {
    std::string offsetWhat = ErrorTextTypedArray::setOffset();
    IndexProofs::ResultBetweenProven between =
        proofs.requireBetween(offset, 0, std::nullopt, offsetWhat);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(offset->loc(), between.error().message);
    }
    offsetUnproven = between.isErr();
  }
  if (offsetUnproven || this->indexProofs->exempt(source->loc(), target)) {
    return;
  }
  std::optional<Linear> at = offset ? proofs.linear(offset) : Linear{Term::zero(), 0};
  if (!proofs.provesSumUpTo(at, proofs.lengthAtMost(source), target)) {
    std::string offsetText = offset ? BytesCheckHelpers::valueText(offset) : std::string();
    std::string message = ErrorTextTypedArray::setFits(
        offsetText, BytesCheckHelpers::valueText(source), BytesCheckHelpers::valueText(target));
    this->state.getDiagnostics().error(source->loc(), message);
  }
}

}
