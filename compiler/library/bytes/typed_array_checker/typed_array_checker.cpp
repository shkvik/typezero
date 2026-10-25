#include "compiler/library/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::string rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

} // namespace

TypedArrayChecker::TypedArrayChecker(Sema &sema) : sema_(sema) {}

Expr *TypedArrayChecker::checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *element = type->elementType();
  llvm::StringRef name = member->name;
  std::string what = "an element of '" + type->name().str() + "'";
  // What toReversed, toSorted and with make: a Uint8Array of a Buffer, as
  // TypedArrayCreateSameType makes.
  const Type *copy = type->isBuffer() ? Type::bytesType(Bytes::Uint8) : type;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;

    return call;
  };
  auto positions = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = sema_.memberChecker().checkPosition(call->args[i]);
    }
  };
  auto mode = [&](int value) {
    auto *lit = sema_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };

  static const llvm::StringLiteral callbacks[] = {"forEach",       "map",  "filter",   "reduce",
                                                  "reduceRight",   "find", "findLast", "findIndex",
                                                  "findLastIndex", "some", "every"};
  if (llvm::is_contained(callbacks, name) ||
      ((name == "sort" || name == "toSorted") && !call->args.empty())) {
    bool arrow = !call->args.empty() && isa<FunctionExpr>(call->args[0]);
    Expr *result = sema_.callbackChecker().checkCallbackMethod(call, member, expected);
    int64_t lo, hi;
    if (name == "map" && !call->args.empty() && !arrow && call->args[0]->type->isFunction() &&
        type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped &&
        type->bytes() != Bytes::Int32) {
      sema_.diags_.error(call->args[0]->loc(),
                   "cannot prove that what the function returns fits in " + what + " (" +
                       rangeText(lo, hi) +
                       "), and JavaScript would cut it without a word; pass an arrow that cuts it "
                       "explicitly, e.g. 'x => " +
                       wrapHint(type->bytes(), "f(x)") + "'");
    }
    if (name == "toSorted" && !call->type->isError()) {
      call->type = copy;
    }
    return result;
  }
  if (name == "flatMap") {
    sema_.diags_.error(member->loc(), "typed arrays have no 'flatMap'; spread the elements into an "
                                "array first, e.g. '[...u].flatMap(f)'");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (name == "at") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    positions(0);
    bool inRange = sema_.indexProofs().provesAtInBounds(member->object, call->args[0]);
    call->mayBeOutOfRange = !inRange;
    return made(Builtin::BytesAt, inRange ? element : Type::optional(element));
  }
  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    Expr *value = sema_.mathChecker().numericArg(call->args[0], element);
    if (!value->type->isNumeric()) {
      return call;
    }
    std::vector<Expr *> args{mode(name == "indexOf"       ? 0
                                  : name == "lastIndexOf" ? 1
                                                          : 2),
                             sema_.typeRelations().convert(value, Type::doubleTy())};
    if (call->args.size() == 2) {
      args.push_back(sema_.memberChecker().checkPosition(call->args[1]));
    }
    call->args = std::move(args);
    return made(Builtin::BytesIndexOf, name == "includes" ? Type::boolTy() : Type::intTy());
  }
  if (name == "join" || name == "toString") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, name == "join" ? 1 : 0)) {
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
    }
    return made(Builtin::BytesJoin, Type::string());
  }
  if (name == "reverse" || name == "sort" || name == "toReversed" || name == "toSorted") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
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
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 3)) {
      return call;
    }
    call->args[0] = sema_.argumentChecker().checkArg(call->args[0], element);
    sema_.byteProofs().checkFits(call->args[0], type, what);
    call->args[0] = sema_.typeRelations().convert(call->args[0], Type::doubleTy());
    positions(1);
    return made(Builtin::BytesFill, type);
  }
  if (name == "set") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      source = sema_.exprChecker().checkExpr(lit, Type::array(element));
      if (source->type->isError()) {
        return call;
      }
      for (const ArrayLitExpr::Element &e : lit->elements) {
        if (!e.spread) {
          sema_.byteProofs().checkFits(e.expr, type, what);
        } else if (!sema_.bytesNewChecker().checkBytesSource(e.expr, type)) {
          return call;
        }
      }
    } else {
      source = sema_.exprChecker().checkExpr(source);
      if (source->type->isIterator()) {
        sema_.diags_.error(source->loc(), "'set' takes an array or a typed array; collect the values "
                                    "of the iterator first, e.g. '[...it]'");
        return call;
      }
      if (!sema_.bytesNewChecker().checkBytesSource(source, type)) {
        return call;
      }
    }
    positions(1);
    return made(Builtin::BytesSet, Type::voidTy());
  }
  if (name == "subarray" || name == "slice") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 2)) {
      return call;
    }
    positions(0);
    return made(name == "subarray" ? Builtin::BytesSubarray : Builtin::BytesSlice, type);
  }
  if (name == "copyWithin") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 3)) {
      return call;
    }
    positions(0);
    return made(Builtin::BytesCopyWithin, type);
  }
  if (name == "with") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    call->args[0] = sema_.memberChecker().checkPosition(call->args[0]);
    call->args[1] = sema_.argumentChecker().checkArg(call->args[1], element);
    sema_.byteProofs().checkFits(call->args[1], type, what);
    call->args[1] = sema_.typeRelations().convert(call->args[1], Type::doubleTy());
    return made(Builtin::BytesWith, copy);
  }
  if (name == "keys" || name == "values" || name == "entries") {
    // Over a copy of the elements, as an array: the same values, unless the
    // loop changes the typed array meanwhile.
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    auto *arrayName = sema_.ctx_.make<NameExpr>(member->loc(), "Array");
    auto *from =
        sema_.ctx_.make<CallExpr>(member->loc(), sema_.ctx_.make<MemberExpr>(member->loc(), arrayName, "from"),
                            std::vector<Expr *>{member->object});
    from->builtin = Builtin::ArrayFrom;
    from->type = Type::array(element);
    auto *iterated = sema_.ctx_.make<MemberExpr>(member->loc(), from, name);
    auto *iterator = sema_.ctx_.make<CallExpr>(call->loc(), iterated, std::vector<Expr *>{});
    return sema_.iteratorMethods().checkArrayIterator(iterator, iterated);
  }
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }
  if (name == "toLocaleString") {
    sema_.diags_.error(member->loc(), "'toLocaleString' of a typed array is not supported; join the "
                                "elements with 'join()', or format each with toLocaleString");
  } else {
    sema_.diags_.error(member->loc(),
                 "property '" + name + "' does not exist on type '" + type->name() + "'");
  }

  return call;
}

Expr *TypedArrayChecker::checkDataViewMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  NumberMethod m;
  if (!numberMethod(name, /*dataView=*/true, m)) {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    sema_.diags_.error(member->loc(), "property '" + name + "' does not exist on type 'DataView'");
    return call;
  }
  if (m.bigint) {
    sema_.decimalChecker().reportBigInt(member->loc(), "'" + name.str() + "'");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  const Type *kindType = Type::bytesType(m.kind);
  const Type *element = kindType->elementType();
  bool oneByte = kindType->elementSize() == 1;
  unsigned min = m.write ? 2 : 1, max = min + (oneByte ? 0 : 1);
  if (!sema_.argumentChecker().checkArity(call, member->loc(), min, max)) {
    return call;
  }
  auto *kind = sema_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(m.kind));
  kind->type = Type::intTy();
  std::vector<Expr *> args{kind, sema_.memberChecker().checkPosition(call->args[0])};
  if (m.write) {
    Expr *value = sema_.argumentChecker().checkArg(call->args[1], element);
    sema_.byteProofs().checkFits(value, kindType, numberWhat(m.kind));
    args.push_back(value);
  }
  if (call->args.size() > min) {
    args.push_back(sema_.argumentChecker().checkArg(call->args[min], Type::boolTy()));
  }
  call->mayBeOutOfRange = !sema_.byteProofs().provesWithin(member->object, args[1], kindType->elementSize());
  call->args = std::move(args);
  call->builtin = m.write ? Builtin::DataViewSet : Builtin::DataViewGet;
  call->type = m.write ? Type::voidTy() : element;

  return call;
}

} // namespace tz
