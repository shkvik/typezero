#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker.h"
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
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TypedArrayChecker::TypedArrayChecker(SemanticState &owner)
    : state(owner) {}

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
  const Type *type = member->object->type;
  const Type *element = type->elementType();
  llvm::StringRef name = member->name;
  std::string what = ErrorTextByteView::element(type->name());
  const Type *copy = type->isBuffer() ? Type::bytesType(Bytes::Uint8) : type;
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  auto positions = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
    }
  };
  auto mode = [&](int value) {
    auto *lit = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };

  static const llvm::StringLiteral callbacks[] = {"forEach",       "map",  "filter",   "reduce",
                                                  "reduceRight",   "find", "findLast", "findIndex",
                                                  "findLastIndex", "some", "every"};
  if (llvm::is_contained(callbacks, name) ||
      ((name == "sort" || name == "toSorted") && !call->args.empty())) {
    bool arrow = !call->args.empty() && isa<FunctionExpr>(call->args[0]);
    Expr *result = CallbackChecker(this->state).checkCallbackMethod(call, member, expected);
    int64_t lo, hi;
    if (name == "map" && !call->args.empty() && !arrow && call->args[0]->type->isFunction() &&
        type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped) {
      std::string range = ErrorTextBytes::range(lo, hi);
      std::string hint = BytesCheckHelpers::wrapHint(type->bytes(), "f(x)");
      std::string message = ErrorTextTypedArray::mapFits(what, range, hint);
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
    }
    if (name == "toSorted" && !call->type->isError()) {
      call->type = copy;
    }
    return result;
  }
  if (name == "flatMap") {
    std::string message = ErrorTextTypedArray::flatMap();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  if (name == "at") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    positions(0);
    bool inRange = IndexProofs(this->state).provesAtInBounds(member->object, call->args[0]);
    call->mayBeOutOfRange = !inRange;
    return made(Builtin::BytesAt, inRange ? element : Type::optional(element));
  }
  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *value = MathChecker(this->state).numericArg(call->args[0], element);
    if (!value->type->isNumeric()) {
      return call;
    }
    std::vector<Expr *> args{mode(name == "indexOf"       ? 0
                                  : name == "lastIndexOf" ? 1
                                                          : 2),
                             TypeRelations(this->state).convert(value, Type::doubleTy())};
    if (call->args.size() == 2) {
      args.push_back(MemberChecker(this->state).checkPosition(call->args[1]));
    }
    call->args = std::move(args);
    return made(Builtin::BytesIndexOf, name == "includes" ? Type::boolTy() : Type::intTy());
  }
  if (name == "join" || name == "toString") {
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, 0, name == "join" ? 1 : 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    }
    return made(Builtin::BytesJoin, Type::string());
  }
  if (name == "reverse" || name == "sort" || name == "toReversed" || name == "toSorted") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (name == "reverse") {
      return made(Builtin::BytesReverse, type);
    }
    if (name == "sort") {
      return made(Builtin::BytesSort, type);
    }
    return made(name == "toReversed" ? Builtin::BytesToReversed : Builtin::BytesToSorted, copy);
  }
  if (name == "fill") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], element);
    ByteProofs::ResultValueFitsType fitting =
        ByteProofs(this->state).valueFits(call->args[0], type, what);
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), fitting.error().message);
    }
    call->args[0] = TypeRelations(this->state).convert(call->args[0], Type::doubleTy());
    positions(1);
    return made(Builtin::BytesFill, type);
  }
  if (name == "set") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      source = ExprChecker(this->state).checkExpr(lit, Type::array(element));
      if (source->type->isError()) {
        return call;
      }
      for (const ArrayLitExpr::Element &e : lit->elements) {
        if (!e.spread) {
          ByteProofs::ResultValueFitsType fitting =
              ByteProofs(this->state).valueFits(e.expr, type, what);
          if (fitting.isErr()) {
            this->state.getDiagnostics().error(e.expr->loc(), fitting.error().message);
          }
        } else if (!BytesNewChecker(this->state).checkBytesSource(e.expr, type)) {
          return call;
        }
      }
    } else {
      source = ExprChecker(this->state).checkExpr(source);
      ResultSetSourceOk set = TypedArrayChecker::setSource(source);
      if (set.isErr()) {
        this->state.getDiagnostics().error(source->loc(), set.error().message);
        return call;
      }
      if (!BytesNewChecker(this->state).checkBytesSource(source, type)) {
        return call;
      }
    }
    positions(1);
    this->checkSetRange(member->object, source, call->args.size() > 1 ? call->args[1] : nullptr);
    return made(Builtin::BytesSet, Type::voidTy());
  }
  if (name == "subarray" || name == "slice") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    positions(0);
    return made(name == "subarray" ? Builtin::BytesSubarray : Builtin::BytesSlice, type);
  }
  if (name == "copyWithin") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    positions(0);
    return made(Builtin::BytesCopyWithin, type);
  }
  if (name == "with") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
    if (!call->args[0]->type->isError() &&
        !IndexProofs(this->state).provesAtInBounds(member->object, call->args[0]) &&
        !IndexProofs(this->state).exempt(call->args[0]->loc(), member->object)) {
      std::string message =
          ErrorTextTypedArray::index(BytesCheckHelpers::valueText(member->object));
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
    }
    call->args[1] = ArgumentChecker(this->state).checkArg(call->args[1], element);
    ByteProofs::ResultValueFitsType fitting =
        ByteProofs(this->state).valueFits(call->args[1], type, what);
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), fitting.error().message);
    }
    call->args[1] = TypeRelations(this->state).convert(call->args[1], Type::doubleTy());
    return made(Builtin::BytesWith, copy);
  }
  if (name == "keys" || name == "values" || name == "entries") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    auto *arrayName = this->state.getAst().make<NameExpr>(member->loc(), "Array");
    auto *from = this->state.getAst().make<CallExpr>(
        member->loc(), this->state.getAst().make<MemberExpr>(member->loc(), arrayName, "from"),
        std::vector<Expr *>{member->object});
    from->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
    from->type = Type::array(element);
    auto *iterated = this->state.getAst().make<MemberExpr>(member->loc(), from, name);
    auto *iterator =
        this->state.getAst().make<CallExpr>(call->loc(), iterated, std::vector<Expr *>{});
    IteratorMethods::ResultArrayIterateCall iterate =
        IteratorMethods(this->state).checkArrayIterator(iterator, iterated);
    if (iterate.isErr()) {
      this->state.getDiagnostics().error(iterated->loc(), iterate.error().message);
      return iterator;
    }
    return iterate.value();
  }
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
  std::string message = name == "toLocaleString"
                            ? ErrorTextTypedArray::locale()
                            : ErrorTextTypedArray::noMember(name, type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *TypedArrayChecker::checkDataViewMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  NumberMethod m;
  if (!BytesCheckHelpers::numberMethod(name, true, m)) {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
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
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  const Type *kindType = Type::bytesType(m.kind);
  const Type *element = kindType->accessType();
  bool oneByte = kindType->elementSize() == 1;
  unsigned min = m.write ? 2 : 1;
  unsigned max = min + (oneByte ? 0 : 1);
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *kind = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(m.kind));
  kind->type = Type::intTy();
  std::vector<Expr *> args{kind, MemberChecker(this->state).checkPosition(call->args[0])};
  if (m.write) {
    Expr *value = ArgumentChecker(this->state).checkArg(call->args[1], element);
    ByteProofs::ResultValueFitsType fitting =
        ByteProofs(this->state).valueFits(value, kindType, BytesCheckHelpers::numberWhat(m.kind));
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(value->loc(), fitting.error().message);
    }
    args.push_back(value);
  }
  if (call->args.size() > min) {
    args.push_back(ArgumentChecker(this->state).checkArg(call->args[min], Type::boolTy()));
  }
  std::string offsetWhat = ErrorTextTypedArray::offsetOf(member->name);
  IndexProofs::ResultUpToWasProven upTo =
      IndexProofs(this->state)
          .requireUpTo(args[1], member->object, kindType->elementSize(), offsetWhat);
  if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(args[1]->loc(), upTo.error().message);
  }
  call->mayBeOutOfRange = upTo.isErr();
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = m.write ? Builtin::DataViewSet : Builtin::DataViewGet};
  call->type = m.write ? Type::voidTy()
               : element->isFloat()
                   ? Type::result(Type::doubleTy(), Prelude(this->state).builtinType("NumberError"))
                   : element;

  return call;
}

void TypedArrayChecker::checkSetRange(const Expr *target, const Expr *source, const Expr *offset) {
  IndexProofs proofs(this->state);
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
  if (offsetUnproven || IndexProofs(this->state).exempt(source->loc(), target)) {
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
