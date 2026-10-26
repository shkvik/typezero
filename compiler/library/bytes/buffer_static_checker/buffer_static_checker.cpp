#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

std::string rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

}

BufferStaticChecker::BufferStaticChecker(Checker &checker) : checker_(checker) {}

Expr *BufferStaticChecker::checkBytesStatic(CallExpr *call, MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !checker_.callChecker().isGlobalName(object, object->name)) {
    return nullptr;
  }
  if (object->name == "BigInt64Array" || object->name == "BigUint64Array") {
    checker_.decimalChecker().reportBigInt(object->loc(), "'" + object->name.str() + "'");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;

    return call;
  };
  if (type->bytes() == Bytes::ArrayBuffer && name == "isView") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    return made(Builtin::ArrayBufferIsView, Type::boolTy());
  }
  if (type->isBuffer() && name != "of") {
    return checkBufferStatic(call, member);
  }
  if (!type->isTypedArray()) {
    checkArgsLoosely();
    checker_.diags_.error(member->loc(), "'" + object->name + "." + name + "' does not exist");
    return call;
  }
  if (name == "of") {
    auto *lit = checker_.ctx_.make<ArrayLitExpr>(call->loc());
    for (Expr *arg : call->args) {
      lit->elements.push_back({arg, false});
    }
    return checker_.bytesNewChecker().checkBytesValues(call, lit, type);
  }
  if (name != "from") {
    checkArgsLoosely();
    checker_.diags_.error(member->loc(), "'" + object->name + "." + name +
                                             "' is not supported; of " + object->name +
                                             " there are 'from' and 'of'");
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  Expr *&source = call->args[0];
  if (call->args.size() == 1) {
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      return checker_.bytesNewChecker().checkBytesValues(call, lit, type);
    }
    checker_.iteratorMethods().markFused(source);
    source = checker_.exprChecker().checkExpr(source);
    if (!checker_.bytesNewChecker().checkBytesSource(source, type)) {
      return call;
    }
    return made(source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray,
                type);
  }

  return checkBytesFromWith(call, type, "'" + object->name.str() + ".from'");
}

Expr *BufferStaticChecker::checkBytesFromWith(CallExpr *call, const Type *type,
                                              const std::string &what) {
  Expr *&source = call->args[0];
  checker_.iteratorMethods().markFused(source);
  source = checker_.exprChecker().checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  const Type *element = GeneratorChecker::iteratedType(from);
  if (!element || from->isString()) {
    checker_.diags_.error(source->loc(),
                          what + " takes an array, a typed array or an iterator, found '" +
                              from->name() + "'");
    return call;
  }
  if (from->isIterator()) {
    checker_.generatorChecker().checkSendsNothing(from, source->loc(), what);
  }
  Checker::Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  sig.ret = type->elementType();
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[1])) {
    Checker::ByteCallback info;
    if (!from->elementRange(info.lo, info.hi)) {
      info.lo = kIntMin;
      info.hi = kIntMax;
    }
    info.target = type;
    checker_.byteCallbacks_[fe->fn] = info;
  }
  bool arrow = isa<FunctionExpr>(call->args[1]);
  call->args[1] = checker_.callbackChecker().checkFunctionArg(call->args[1], sig, 1, what);
  if (!call->args[1]->type->isFunction()) {
    return call;
  }
  int64_t lo, hi;
  if (!arrow && type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped &&
      type->bytes() != Bytes::Int32) {
    checker_.diags_.error(
        call->args[1]->loc(),
        "cannot prove that what the function returns fits in an element of '" + type->name() +
            "' (" + rangeText(lo, hi) +
            "), and JavaScript would cut it without a word; pass an arrow that cuts it "
            "explicitly, e.g. 'x => " +
            wrapHint(type->bytes(), "f(x)") + "'");
    return call;
  }
  checker_.flowChecker().calledFunction();
  auto *arrayFrom = checker_.ctx_.make<CallExpr>(
      call->loc(),
      checker_.ctx_.make<MemberExpr>(call->loc(),
                                     checker_.ctx_.make<NameExpr>(call->loc(), "Array"), "from"),
      std::vector<Expr *>{source, call->args[1]});
  arrayFrom->builtin = Builtin::ArrayFrom;
  arrayFrom->type = Type::array(type->elementType());
  call->args = {arrayFrom};
  call->builtin = Builtin::BytesFromArray;
  call->type = type;

  return call;
}

Expr *BufferStaticChecker::checkBufferStatic(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;

    return call;
  };
  auto boolLit = [&](bool value) {
    auto *lit = checker_.ctx_.make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto utf8Lit = [&] {
    auto *lit = checker_.ctx_.make<IntLitExpr>(call->loc(), Utf8);
    lit->type = Type::intTy();

    return lit;
  };
  if (name == "from") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 3)) {
      return call;
    }
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && call->args.size() == 1) {
      return checker_.bytesNewChecker().checkBytesValues(call, lit, buffer);
    }
    checker_.iteratorMethods().markFused(source);
    source = checker_.exprChecker().checkExpr(source);
    const Type *from = source->type;
    if (from->isError()) {
      return call;
    }
    if (from->isString()) {
      if (call->args.size() > 2) {
        checker_.argumentChecker().checkArity(call, member->loc(), 1, 2);
        return call;
      }
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? checker_.bytesChecker().checkBufferEncoding(call->args[1]) : utf8Lit();
      return made(Builtin::BufferFromString, buffer);
    }
    if (from->bytes() == Bytes::ArrayBuffer) {
      for (size_t i = 1; i < call->args.size(); ++i) {
        call->args[i] = checker_.memberChecker().checkPosition(call->args[i]);
      }
      return made(Builtin::BytesView, buffer);
    }
    if (call->args.size() > 1) {
      checker_.diags_.error(call->args[1]->loc(),
                            "'Buffer.from' takes an offset and a length only with "
                            "an ArrayBuffer, found '" +
                                from->name() + "'");
      return call;
    }
    if (!checker_.bytesNewChecker().checkBytesSource(source, buffer)) {
      return call;
    }
    return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, buffer);
  }
  if (name == "alloc") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 3)) {
      return call;
    }
    call->args[0] = checker_.memberChecker().checkPosition(call->args[0]);
    if (std::optional<Linear> n = checker_.indexProofs().linear(call->args[0]);
        n && n->term.isZero() && n->offset < 0) {
      checker_.diags_.error(call->args[0]->loc(), "the size of a Buffer cannot be negative");
    }
    if (call->args.size() == 1) {
      call->args.push_back(boolLit(true));
      return made(Builtin::BytesNew, buffer);
    }
    Expr *fill = checker_.exprChecker().checkExpr(call->args[1]);
    const Type *t = fill->type;
    if (t->isError()) {
      return call;
    }
    if (t->isInteger()) {
      fill = checker_.typeRelations().convert(fill, Type::intTy() == t ? t : Type::intTy());
      checker_.byteProofs().checkFits(fill, buffer, "a byte");
    } else if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      fill = checker_.typeRelations().convert(fill, uint8);
    } else if (!t->isString()) {
      checker_.diags_.error(fill->loc(),
                            "'Buffer.alloc' fills with a number, a string or a Uint8Array, "
                            "found '" +
                                t->name() + "'");
      return call;
    }
    Expr *encoding = utf8Lit();
    if (call->args.size() == 3) {
      encoding = checker_.bytesChecker().checkBufferEncoding(call->args[2]);
      if (!t->isString()) {
        checker_.diags_.error(encoding->loc(), "an encoding goes only with a string to fill with");
      }
    }
    call->args = {call->args[0], fill, encoding};
    return made(Builtin::BufferAllocFill, buffer);
  }
  if (name == "allocUnsafe" || name == "allocUnsafeSlow") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = checker_.memberChecker().checkPosition(call->args[0]);
    call->args.push_back(boolLit(false));
    return made(Builtin::BytesNew, buffer);
  }
  if (name == "concat") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    Expr *&list = call->args[0];
    list = checker_.exprChecker().checkExpr(list, Type::array(uint8));
    const Type *t = list->type;
    if (t->isError()) {
      return call;
    }
    if (!t->isArray() || (t->inner() != uint8 && t->inner() != buffer)) {
      checker_.diags_.error(list->loc(),
                            "'Buffer.concat' takes an array of Uint8Arrays or Buffers, "
                            "found '" +
                                t->name() + "'");
      return call;
    }
    if (call->args.size() == 2) {
      call->args[1] = checker_.memberChecker().checkPosition(call->args[1]);
    }
    return made(Builtin::BufferConcat, buffer);
  }
  if (name == "byteLength") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    const Type *t = call->args[0]->type;
    if (t->isError()) {
      return call;
    }
    if (t->isString()) {
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? checker_.bytesChecker().checkBufferEncoding(call->args[1]) : utf8Lit();
    } else if (!t->isByteView() && t->bytes() != Bytes::ArrayBuffer) {
      checker_.diags_.error(call->args[0]->loc(),
                            "'Buffer.byteLength' takes a string, a typed array, a "
                            "DataView or an ArrayBuffer, found '" +
                                t->name() + "'");
      return call;
    }
    return made(Builtin::BufferByteLength, Type::intTy());
  }
  if (name == "compare") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = checker_.argumentChecker().checkArg(arg, uint8);
    }
    return made(Builtin::BufferCompare, Type::intTy());
  }
  if (name == "isBuffer") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    const Type *arg = call->args[0]->type, *base = arg->nonOptional();
    auto mayBe = [&](const Type *t) { return t->isBuffer() || t->bytes() == Bytes::Uint8; };
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    if (!arg->isError() && llvm::any_of(members, mayBe) &&
        (arg->isOptional() ||
         !llvm::all_of(members, [](const Type *t) { return t->isBuffer(); }))) {
      auto *rhs = checker_.ctx_.make<NameExpr>(member->loc(), "Buffer");
      rhs->type = Type::bytesType(Bytes::Buffer);
      auto *test =
          checker_.ctx_.make<BinaryExpr>(call->loc(), BinaryOp::InstanceOf, call->args[0], rhs);
      test->type = Type::boolTy();
      return test;
    }
    return made(Builtin::BufferIsBuffer, Type::boolTy());
  }
  if (name == "isEncoding") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    if (std::optional<std::u16string> text = literalText(call->args[0])) {
      return boolLit(encodingNamed(utf8(*text)).has_value());
    }
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    return made(Builtin::BufferIsEncoding, Type::boolTy());
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }
  checker_.diags_.error(member->loc(),
                        "'Buffer." + name +
                            "' is not supported; Buffer has from, alloc, "
                            "allocUnsafe, concat, byteLength, compare, isBuffer, isEncoding "
                            "and of");

  return call;
}

Expr *BufferStaticChecker::checkEmbedBytes(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (!checker_.argumentChecker().checkArity(call, name->loc(), 1, 1)) {
    return call;
  }
  std::string bytes, shown, full;
  if (!checker_.jsonStaticChecker().readEmbedded(call, "embedBytes", bytes, shown, full)) {
    return call;
  }
  if (int64_t(bytes.size()) > kMaxLength) {
    checker_.diags_.error(call->args[0]->loc(), "'" + shown +
                                                    "' is larger than the 1 GiB a Uint8Array "
                                                    "may have");
    return call;
  }
  llvm::SMLoc loc = call->args[0]->loc();
  std::u16string units(bytes.size(), u'\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    units[i] = char16_t(static_cast<unsigned char>(bytes[i]));
  }
  auto *lit = checker_.ctx_.make<StringLitExpr>(loc, std::move(units));
  lit->type = Type::string();
  llvm::SmallVector<llvm::UTF16, 64> path;
  llvm::convertUTF8ToUTF16String(full, path);
  auto *file = checker_.ctx_.make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {lit, file};
  call->builtin = Builtin::EmbedBytes;
  call->type = Type::bytesType(Bytes::Uint8);

  return call;
}

}
