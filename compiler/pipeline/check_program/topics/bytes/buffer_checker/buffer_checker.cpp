#include "compiler/pipeline/check_program/topics/bytes/buffer_checker/buffer_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;

namespace tz {

std::optional<std::u16string> BufferChecker::literalText(const Expr *e) {
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

std::string BufferChecker::rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

BufferChecker::BufferChecker(CheckState &owner)
    : state(owner) {}

Expr *BufferChecker::checkBufferMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  auto intLit = [&](int64_t value) {
    auto *lit = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();

    return lit;
  };
  auto boolLit = [&](bool value) {
    auto *lit = this->state.getAst().make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto bytesArg = [&](Expr *arg) { return ArgumentChecker(this->state).checkArg(arg, uint8); };
  auto valueArg = [&](Expr *arg) -> Expr * {
    arg = ExprChecker(this->state).checkExpr(arg, nullptr);
    const Type *t = arg->type;
    if (t->isError() || t->isString()) {
      return arg;
    }
    if (t->isInteger()) {
      return TypeRelations(this->state).convert(arg, Type::intTy() == t ? t : Type::intTy());
    }
    if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      return TypeRelations(this->state).convert(arg, uint8);
    }
    this->state.getDiagnostics().error(
        arg->loc(),
        "'" + name + "' takes a number, a string or a Uint8Array, found '" + t->name() + "'");
    arg->type = Type::error();

    return arg;
  };
  auto utf8Lit = [&] { return intLit(Utf8); };

  if (name == "slice") {
    this->state.getDiagnostics().error(
        member->loc(), "'slice' of a Buffer is not supported: in node it shares the "
                       "memory, unlike 'slice' of a Uint8Array, and is deprecated; use "
                       "'subarray(start, end)' for the same memory, or "
                       "'Buffer.from(b.subarray(start, end))' for a copy");
    checkArgsLoosely();
    return call;
  }
  if (name == "toString" || name == "toLocaleString") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    std::vector<Expr *> args{call->args.empty()
                                 ? utf8Lit()
                                 : BytesChecker(this->state).checkBufferEncoding(call->args[0])};
    for (size_t i = 1; i < call->args.size(); ++i) {
      args.push_back(MemberChecker(this->state).checkPosition(call->args[i]));
    }
    call->args = std::move(args);
    return made(Builtin::BufferToString, Type::string());
  }
  if (name == "toJSON") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    return made(Builtin::BufferToJSON, Type::object({{"type", Type::string(), false},
                                                     {"data", Type::array(Type::intTy()), false}}));
  }
  if (name == "equals") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    return made(Builtin::BufferEquals, Type::boolTy());
  }
  if (name == "compare") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
    }
    IndexProofs proofs(this->state);
    static const char *const names[] = {"", "targetStart", "targetEnd", "sourceStart", "sourceEnd"};
    for (size_t i = 1; i < call->args.size(); ++i) {
      std::string what = std::string("'") + names[i] + "'";
      if (i == 2 || i == 4) {
        IndexProofs::ResultUpToWasProven upTo =
            proofs.requireUpTo(call->args[i], i == 2 ? call->args[0] : member->object, 0, what);
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(call->args[i]->loc(), upTo.error().message);
        }
      } else {
        IndexProofs::ResultBetweenProven between =
            proofs.requireBetween(call->args[i], 0, std::nullopt, what);
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(call->args[i]->loc(), between.error().message);
        }
      }
    }
    return made(Builtin::BufferCompareTo, Type::intTy());
  }
  if (name == "copy") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
    }
    IndexProofs proofs(this->state);
    static const char *const names[] = {"", "targetStart", "sourceStart", "sourceEnd"};
    for (size_t i = 1; i < call->args.size(); ++i) {
      std::string what = std::string("'") + names[i] + "'";
      if (i == 2) {
        IndexProofs::ResultUpToWasProven upTo =
            proofs.requireUpTo(call->args[i], member->object, 0, what);
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(call->args[i]->loc(), upTo.error().message);
        }
      } else {
        IndexProofs::ResultBetweenProven between =
            proofs.requireBetween(call->args[i], 0, std::nullopt, what);
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(call->args[i]->loc(), between.error().message);
        }
      }
    }
    return made(Builtin::BufferCopy, Type::intTy());
  }
  if (name == "write") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *text = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      if (BufferChecker::literalText(arg) || i + 1 == call->args.size()) {
        Expr *checked = ExprChecker(this->state).checkExpr(arg);
        if (checked->type->isString()) {
          encoding = BytesChecker(this->state).checkBufferEncoding(checked);
          if (i + 1 != call->args.size()) {
            this->state.getDiagnostics().error(call->args[i + 1]->loc(),
                                               "the encoding comes last in 'write'");
          }
          break;
        }
        positions.push_back(MemberChecker(this->state).checkPosition(checked));
        continue;
      }
      positions.push_back(MemberChecker(this->state).checkPosition(arg));
    }
    if (!positions.empty()) {
      IndexProofs::ResultUpToWasProven upTo =
          IndexProofs(this->state).requireUpTo(positions[0], member->object, 0, "the offset");
      if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(positions[0]->loc(), upTo.error().message);
      }
      if (upTo.isOk() && positions.size() > 1) {
        this->checkWriteLength(member->object, positions[0], positions[1]);
      }
    }
    std::vector<Expr *> args{text, encoding ? encoding : utf8Lit()};
    args.insert(args.end(), positions.begin(), positions.end());
    call->args = std::move(args);
    return made(Builtin::BufferWrite, Type::intTy());
  }
  if (name == "fill" || name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    bool fill = name == "fill";
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, 1, fill ? 4 : 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *value = valueArg(call->args[0]);
    if (value->type->isError()) {
      return call;
    }
    if (fill && value->type->isInt()) {
      ByteProofs(this->state).checkFits(value, type, "a byte");
    }
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *checked = ExprChecker(this->state).checkExpr(call->args[i]);
      if (checked->type->isString()) {
        encoding = BytesChecker(this->state).checkBufferEncoding(checked);
        if (i + 1 != call->args.size()) {
          this->state.getDiagnostics().error(call->args[i + 1]->loc(),
                                             "the encoding comes last in '" + name + "'");
        }
        break;
      }
      positions.push_back(MemberChecker(this->state).checkPosition(checked));
    }
    if (encoding && !value->type->isString()) {
      this->state.getDiagnostics().error(encoding->loc(),
                                         "an encoding goes only with a string value");
    }
    if (fill) {
      for (size_t i = 0; i < positions.size(); ++i) {
        IndexProofs::ResultUpToWasProven upTo =
            IndexProofs(this->state)
                .requireUpTo(positions[i], member->object, 0, i == 0 ? "the offset" : "the end");
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(positions[i]->loc(), upTo.error().message);
        }
      }
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
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args = {intLit(name == "swap16" ? 2 : name == "swap32" ? 4 : 8)};
    return made(Builtin::BufferSwap, type);
  }
  NumberMethod m;
  if (!numberMethod(name, false, m)) {
    return nullptr;
  }
  if (m.bigint) {
    DecimalChecker(this->state).reportBigInt(member->loc(), "'" + name.str() + "'");
    checkArgsLoosely();
    return call;
  }
  Bytes kind = m.kind;
  if (m.variable) {
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, m.write ? 3 : 2, m.write ? 3 : 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    std::vector<Expr *> args{boolLit(m.isSigned), boolLit(m.big)};
    size_t at = 0;
    if (m.write) {
      Expr *value = ArgumentChecker(this->state).checkArg(call->args[0], Type::intTy());
      args.push_back(value);
      at = 1;
    }
    Expr *offset = MemberChecker(this->state).checkPosition(call->args[at]);
    Expr *count = MemberChecker(this->state).checkPosition(call->args[at + 1]);
    args.push_back(offset);
    args.push_back(count);
    std::optional<int64_t> n = ConstantFolder(this->state).intConstant(count);
    if (!n || *n < 1 || *n > 6) {
      this->state.getDiagnostics().error(
          count->loc(), "the byte count of '" + name + "' must be written as a number from 1 to 6");
      return call;
    }
    IndexProofs::ResultUpToWasProven upTo =
        IndexProofs(this->state)
            .requireUpTo(offset, member->object, *n, "the offset of '" + name.str() + "'");
    if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(offset->loc(), upTo.error().message);
    }
    call->mayBeOutOfRange = upTo.isErr();
    if (m.write) {
      int64_t bits = *n * 8;
      int64_t lo = m.isSigned ? -(int64_t(1) << (bits - 1)) : 0;
      int64_t hi = m.isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
      if (!args[2]->type->isError() && !ByteProofs(this->state).provesFits(args[2], lo, hi)) {
        this->state.getDiagnostics().error(
            args[2]->loc(), "cannot prove that the value fits in " + std::to_string(*n) +
                                " bytes (" + BufferChecker::rangeText(lo, hi) +
                                "), and node would throw a RangeError; check it first");
      }
    }
    call->args = std::move(args);
    return made(m.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar, Type::intTy());
  }
  const Type *element = Type::bytesType(kind)->accessType();
  int64_t size = Type::bytesType(kind)->elementSize();
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, m.write ? 1 : 0, m.write ? 2 : 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  std::vector<Expr *> args{intLit(int64_t(kind)), boolLit(m.big)};
  size_t at = 0;
  if (m.write) {
    Expr *value = ArgumentChecker(this->state).checkArg(call->args[0], element);
    ByteProofs(this->state).checkFits(value, Type::bytesType(kind), numberWhat(kind));
    args.push_back(value);
    at = 1;
  }
  Expr *offset =
      call->args.size() > at ? MemberChecker(this->state).checkPosition(call->args[at]) : nullptr;
  if (offset) {
    args.push_back(offset);
  }
  IndexProofs::ResultUpToWasProven upTo =
      IndexProofs(this->state)
          .requireUpTo(offset, member->object, size, "the offset of '" + name.str() + "'");
  if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(offset ? offset->loc() : member->object->loc(),
                                       upTo.error().message);
  }
  call->mayBeOutOfRange = upTo.isErr();
  call->args = std::move(args);

  const Type *read =
      element->isFloat()
          ? Type::result(Type::doubleTy(), Prelude(this->state).builtinType("NumberError"))
          : element;

  return made(m.write ? Builtin::BufferWriteNumber : Builtin::BufferRead,
              m.write ? Type::intTy() : read);
}

Expr *BufferChecker::checkTextMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (type->bytes() == Bytes::TextEncoder && name == "encode") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::TextEncode};
    call->type = Type::bytesType(Bytes::Uint8);
    return call;
  }
  if (type->bytes() == Bytes::TextEncoder && name == "encodeInto") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    call->args[1] =
        ArgumentChecker(this->state).checkArg(call->args[1], Type::bytesType(Bytes::Uint8));
    call->target = BuiltinCall{.builtin = Builtin::TextEncodeInto};
    call->type = Type::object({{"read", Type::intTy(), false}, {"written", Type::intTy(), false}});
    return call;
  }
  if ((type->bytes() == Bytes::TextDecoder || type->bytes() == Bytes::FatalTextDecoder) &&
      name == "decode") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      Expr *input = call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
      const Type *t = input->type;
      if (!t->isError() && !t->isUndefined() && !t->isByteView() &&
          t->bytes() != Bytes::ArrayBuffer) {
        this->state.getDiagnostics().error(
            input->loc(), "'decode' takes a typed array, a DataView or an ArrayBuffer, "
                          "found '" +
                              t->name() + "'");
        return call;
      }
    }
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"stream", Type::optional(Type::boolTy()), false}});
      Expr *given = ExprChecker(this->state).checkExpr(call->args[1], options);
      call->args[1] = TypeRelations(this->state).coerce(given, options, "the options of 'decode'");
    }
    call->target = BuiltinCall{.builtin = Builtin::TextDecode};
    call->type = type->bytes() == Bytes::FatalTextDecoder
                     ? Type::result(Type::string(), Prelude(this->state).builtinType("DecodeError"))
                     : Type::string();
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'");

  return call;
}

void BufferChecker::checkWriteLength(const Expr *object, const Expr *offset, const Expr *length) {
  IndexProofs proofs(this->state);
  IndexProofs::ResultBetweenProven between =
      proofs.requireBetween(length, 0, std::nullopt, "the length");
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(length->loc(), between.error().message);
  }
  if (between.isErr() || IndexProofs(this->state).exempt(length->loc(), object)) {
    return;
  }
  if (!proofs.provesSumUpTo(proofs.linear(offset), proofs.linear(length), object)) {
    this->state.getDiagnostics().error(length->loc(),
                                       "cannot prove that '" + valueText(offset) + " + " +
                                           valueText(length) + " <= " + valueText(object) +
                                           ".length', and node would throw a RangeError; check "
                                           "it first, with the offset or the length written as "
                                           "a number");
  }
}

}
