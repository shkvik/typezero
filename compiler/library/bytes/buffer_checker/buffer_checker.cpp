#include "compiler/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
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

// Text known when compiling: a literal, or a template without values.
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

} // namespace

BufferChecker::BufferChecker(Sema &sema) : sema_(sema) {}

Expr *BufferChecker::checkBufferMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;

    return call;
  };
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  auto intLit = [&](int64_t value) {
    auto *lit = sema_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };
  auto boolLit = [&](bool value) {
    auto *lit = sema_.ctx_.make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto bytesArg = [&](Expr *arg) { return sema_.argumentChecker().checkArg(arg, uint8); };
  // A Uint8Array, a string or a number, which indexOf and fill take.
  auto valueArg = [&](Expr *arg) -> Expr * {
    arg = sema_.exprChecker().checkExpr(arg, nullptr);
    const Type *t = arg->type;
    if (t->isError() || t->isString()) {
      return arg;
    }
    if (t->isInteger()) {
      return sema_.typeRelations().convert(arg, Type::intTy() == t ? t : Type::intTy());
    }
    if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      return sema_.typeRelations().convert(arg, uint8);
    }
    sema_.diags_.error(arg->loc(), "'" + name + "' takes a number, a string or a Uint8Array, found '" +
                                 t->name() + "'");
    arg->type = Type::error();

    return arg;
  };
  auto utf8Lit = [&] { return intLit(Utf8); };

  if (name == "slice") {
    sema_.diags_.error(member->loc(), "'slice' of a Buffer is not supported: in node it shares the "
                                "memory, unlike 'slice' of a Uint8Array, and is deprecated; use "
                                "'subarray(start, end)' for the same memory, or "
                                "'Buffer.from(b.subarray(start, end))' for a copy");
    checkArgsLoosely();
    return call;
  }
  if (name == "toString" || name == "toLocaleString") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 3)) {
      return call;
    }
    std::vector<Expr *> args{call->args.empty() ? utf8Lit() : sema_.bytesChecker().checkBufferEncoding(call->args[0])};
    for (size_t i = 1; i < call->args.size(); ++i) {
      args.push_back(sema_.memberChecker().checkPosition(call->args[i]));
    }
    call->args = std::move(args);
    return made(Builtin::BufferToString, Type::string());
  }
  if (name == "toJSON") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    return made(Builtin::BufferToJSON, Type::object({{"type", Type::string(), false},
                                                     {"data", Type::array(Type::intTy()), false}}));
  }
  if (name == "equals") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    return made(Builtin::BufferEquals, Type::boolTy());
  }
  if (name == "compare") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 5)) {
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = sema_.memberChecker().checkPosition(call->args[i]);
    }
    return made(Builtin::BufferCompareTo, Type::intTy());
  }
  if (name == "copy") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 4)) {
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = sema_.memberChecker().checkPosition(call->args[i]);
    }
    return made(Builtin::BufferCopy, Type::intTy());
  }
  if (name == "write") {
    // write(string, offset?, length?, encoding?), where the encoding may come
    // in place of the offset or the length, as node reads it.
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 4)) {
      return call;
    }
    Expr *text = sema_.argumentChecker().checkArg(call->args[0], Type::string());
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      if (literalText(arg) || i + 1 == call->args.size()) {
        Expr *checked = sema_.exprChecker().checkExpr(arg);
        if (checked->type->isString()) {
          encoding = sema_.bytesChecker().checkBufferEncoding(checked);
          if (i + 1 != call->args.size()) {
            sema_.diags_.error(call->args[i + 1]->loc(), "the encoding comes last in 'write'");
          }
          break;
        }
        positions.push_back(sema_.memberChecker().checkPosition(checked));
        continue;
      }
      positions.push_back(sema_.memberChecker().checkPosition(arg));
    }
    std::vector<Expr *> args{text, encoding ? encoding : utf8Lit()};
    args.insert(args.end(), positions.begin(), positions.end());
    call->args = std::move(args);
    return made(Builtin::BufferWrite, Type::intTy());
  }
  if (name == "fill" || name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    // fill(value, offset?, end?, encoding?) and indexOf(value, byteOffset?,
    // encoding?), where an encoding may come in place of a position.
    bool fill = name == "fill";
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, fill ? 4 : 3)) {
      return call;
    }
    Expr *value = valueArg(call->args[0]);
    if (value->type->isError()) {
      return call;
    }
    if (fill && value->type->isInt()) {
      sema_.byteProofs().checkFits(value, type, "a byte");
    }
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *checked = sema_.exprChecker().checkExpr(call->args[i]);
      if (checked->type->isString()) {
        encoding = sema_.bytesChecker().checkBufferEncoding(checked);
        if (i + 1 != call->args.size()) {
          sema_.diags_.error(call->args[i + 1]->loc(), "the encoding comes last in '" + name + "'");
        }
        break;
      }
      positions.push_back(sema_.memberChecker().checkPosition(checked));
    }
    if (encoding && !value->type->isString()) {
      sema_.diags_.error(encoding->loc(), "an encoding goes only with a string value");
    }
    std::vector<Expr *> args;
    if (!fill) {
      args.push_back(intLit(name == "indexOf" ? 0 : name == "lastIndexOf" ? 1 : 2));
    }
    args.push_back(value);
    args.push_back(encoding ? encoding : utf8Lit());
    args.insert(args.end(), positions.begin(), positions.end());
    call->args = std::move(args);
    if (fill) {
      return made(Builtin::BufferFill, type);
    }
    return made(Builtin::BufferIndexOf, name == "includes" ? Type::boolTy() : Type::intTy());
  }
  if (name == "swap16" || name == "swap32" || name == "swap64") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->args = {intLit(name == "swap16" ? 2 : name == "swap32" ? 4 : 8)};
    return made(Builtin::BufferSwap, type);
  }
  NumberMethod m;
  if (!numberMethod(name, /*dataView=*/false, m)) {
    return nullptr; // the methods of Uint8Array
  }
  if (m.bigint) {
    sema_.decimalChecker().reportBigInt(member->loc(), "'" + name.str() + "'");
    checkArgsLoosely();
    return call;
  }
  Bytes kind = m.kind;
  if (m.variable) {
    // readUIntLE(offset, byteLength) and writeUIntLE(value, offset,
    // byteLength): up to 6 bytes, a long.
    if (!sema_.argumentChecker().checkArity(call, member->loc(), m.write ? 3 : 2, m.write ? 3 : 2)) {
      return call;
    }
    std::vector<Expr *> args{boolLit(m.isSigned), boolLit(m.big)};
    size_t at = 0;
    if (m.write) {
      Expr *value = sema_.argumentChecker().checkArg(call->args[0], Type::longTy());
      args.push_back(value);
      at = 1;
    }
    Expr *offset = sema_.memberChecker().checkPosition(call->args[at]);
    Expr *count = sema_.memberChecker().checkPosition(call->args[at + 1]);
    args.push_back(offset);
    args.push_back(count);
    if (m.write) {
      std::optional<int64_t> n = sema_.constantFolder().intConstant(count);
      if (!n || *n < 1 || *n > 6) {
        sema_.diags_.error(count->loc(), "the byte count of '" + name +
                                       "' must be written as a number from 1 to 6, so that the "
                                       "value can be shown to fit");
        return call;
      }
      int64_t bits = *n * 8;
      int64_t lo = m.isSigned ? -(int64_t(1) << (bits - 1)) : 0;
      int64_t hi = m.isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
      if (!args[2]->type->isError() && !sema_.byteProofs().provesFits(args[2], lo, hi)) {
        sema_.diags_.error(args[2]->loc(), "cannot prove that the value fits in " + std::to_string(*n) +
                                         " bytes (" + rangeText(lo, hi) +
                                         "), and node would throw a RangeError; check it first");
      }
    }
    call->args = std::move(args);
    call->mayBeOutOfRange = true;
    return made(m.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar,
                m.write ? Type::intTy() : Type::longTy());
  }
  const Type *element = Type::bytesType(kind)->elementType();
  int64_t size = Type::bytesType(kind)->elementSize();
  if (!sema_.argumentChecker().checkArity(call, member->loc(), m.write ? 1 : 0, m.write ? 2 : 1)) {
    return call;
  }
  std::vector<Expr *> args{intLit(int64_t(kind)), boolLit(m.big)};
  size_t at = 0;
  if (m.write) {
    Expr *value = sema_.argumentChecker().checkArg(call->args[0], element);
    sema_.byteProofs().checkFits(value, Type::bytesType(kind), numberWhat(kind));
    args.push_back(value);
    at = 1;
  }
  Expr *offset = call->args.size() > at ? sema_.memberChecker().checkPosition(call->args[at]) : nullptr;
  if (offset) {
    args.push_back(offset);
  }
  call->mayBeOutOfRange = !sema_.byteProofs().provesWithin(member->object, offset, size);
  call->args = std::move(args);

  return made(m.write ? Builtin::BufferWriteNumber : Builtin::BufferRead,
              m.write ? Type::intTy() : element);
}

Expr *BufferChecker::checkTextMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (type->bytes() == Bytes::TextEncoder && name == "encode") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
    }
    call->builtin = Builtin::TextEncode;
    call->type = Type::bytesType(Bytes::Uint8);
    return call;
  }
  if (type->bytes() == Bytes::TextEncoder && name == "encodeInto") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
    call->args[1] = sema_.argumentChecker().checkArg(call->args[1], Type::bytesType(Bytes::Uint8));
    call->builtin = Builtin::TextEncodeInto;
    call->type = Type::object({{"read", Type::intTy(), false}, {"written", Type::intTy(), false}});
    return call;
  }
  if (type->bytes() == Bytes::TextDecoder && name == "decode") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 2)) {
      return call;
    }
    if (!call->args.empty()) {
      // `decode(undefined, { stream: false })` ends a stream with no bytes.
      Expr *input = call->args[0] = sema_.exprChecker().checkExpr(call->args[0]);
      const Type *t = input->type;
      if (!t->isError() && !t->isUndefined() && !t->isByteView() &&
          t->bytes() != Bytes::ArrayBuffer) {
        sema_.diags_.error(input->loc(), "'decode' takes a typed array, a DataView or an ArrayBuffer, "
                                   "found '" +
                                       t->name() + "'");
        return call;
      }
    }
    // `{ stream: true }`: the bytes go on in the next call, which may end a
    // character they begin (docs/bytes.md, section 5).
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"stream", Type::optional(Type::boolTy()), false}});
      Expr *given = sema_.exprChecker().checkExpr(call->args[1], options);
      call->args[1] = sema_.typeRelations().coerce(given, options, "the options of 'decode'");
    }
    call->builtin = Builtin::TextDecode;
    call->type = Type::string();
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }
  sema_.diags_.error(member->loc(),
               "property '" + name + "' does not exist on type '" + type->name() + "'");

  return call;
}

} // namespace tz
