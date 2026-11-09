#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;

namespace tz {

BufferChecker::BufferChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.getPrelude();
  this->byteProofs = &all.getByteProofs();
  this->exprChecker = &all.getExprChecker();
  this->indexProofs = &all.getIndexProofs();
  this->bytesChecker = &all.getBytesChecker();
  this->memberChecker = &all.getMemberChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
  this->argumentChecker = &all.getArgumentChecker();
}

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

BufferChecker::ResultCountOfBytes BufferChecker::countOfBytes(std::optional<int64_t> count,
                                                              llvm::StringRef name) {
  if (!count || *count < 1 || *count > 6) {
    std::string message = ErrorTextBuffer::byteCount(name);

    return err(BufferCheckerCountOfBytesError{
        .code = BufferCheckerCountOfBytesErrorCode::NOT_ONE_TO_SIX,
        .message = message,
    });
  }

  return ok(*count);
}

BufferChecker::ResultDecodedInput BufferChecker::decodedInput(const Type *type) {
  if (!type->isError() && !type->isUndefined() && !type->isByteView() &&
      type->bytes() != Bytes::ArrayBuffer) {
    std::string message = ErrorTextBuffer::decode(type->name());

    return err(BufferCheckerDecodedInputError{
        .code = BufferCheckerDecodedInputErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

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
      arg = this->exprChecker->checkExpr(arg);
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
  auto bytesArg = [&](Expr *arg) { return this->argumentChecker->checkArg(arg, uint8); };
  auto valueArg = [&](Expr *arg) -> Expr * {
    arg = this->exprChecker->checkExpr(arg, nullptr);
    const Type *t = arg->type;
    if (t->isError() || t->isString()) {
      return arg;
    }
    if (t->isInteger()) {
      return this->typeRelations->convert(arg, Type::intTy() == t ? t : Type::intTy());
    }
    if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      return this->typeRelations->convert(arg, uint8);
    }
    std::string message = ErrorTextBuffer::valueArg(name, t->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    arg->type = Type::error();

    return arg;
  };
  auto utf8Lit = [&] { return intLit(Utf8); };

  if (name == "slice") {
    std::string message = ErrorTextBuffer::slice();
    this->state.getDiagnostics().error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  if (name == "toString" || name == "toLocaleString") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    std::vector<Expr *> args{
        call->args.empty() ? utf8Lit() : this->bytesChecker->checkBufferEncoding(call->args[0])};
    for (size_t i = 1; i < call->args.size(); ++i) {
      args.push_back(this->memberChecker->checkPosition(call->args[i]));
    }
    call->args = std::move(args);
    return made(Builtin::BufferToString, Type::string());
  }
  if (name == "toJSON") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    return made(Builtin::BufferToJSON, Type::object({{"type", Type::string(), false},
                                                     {"data", Type::array(Type::intTy()), false}}));
  }
  if (name == "equals") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    return made(Builtin::BufferEquals, Type::boolTy());
  }
  if (name == "compare") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = this->memberChecker->checkPosition(call->args[i]);
    }
    IndexProofs &proofs = *this->indexProofs;
    static const char *const names[] = {"", "targetStart", "targetEnd", "sourceStart", "sourceEnd"};
    for (size_t i = 1; i < call->args.size(); ++i) {
      std::string what = ErrorTextBuffer::position(names[i]);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = this->memberChecker->checkPosition(call->args[i]);
    }
    IndexProofs &proofs = *this->indexProofs;
    static const char *const names[] = {"", "targetStart", "sourceStart", "sourceEnd"};
    for (size_t i = 1; i < call->args.size(); ++i) {
      std::string what = ErrorTextBuffer::position(names[i]);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *text = this->argumentChecker->checkArg(call->args[0], Type::string());
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      if (BufferChecker::literalText(arg) || i + 1 == call->args.size()) {
        Expr *checked = this->exprChecker->checkExpr(arg);
        if (checked->type->isString()) {
          encoding = this->bytesChecker->checkBufferEncoding(checked);
          if (i + 1 != call->args.size()) {
            std::string message = ErrorTextBuffer::encoding("write");
            this->state.getDiagnostics().error(call->args[i + 1]->loc(), message);
          }
          break;
        }
        positions.push_back(this->memberChecker->checkPosition(checked));
        continue;
      }
      positions.push_back(this->memberChecker->checkPosition(arg));
    }
    if (!positions.empty()) {
      std::string offsetWhat = ErrorTextBufferLabel::offset();
      IndexProofs::ResultUpToWasProven upTo =
          this->indexProofs->requireUpTo(positions[0], member->object, 0, offsetWhat);
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
        this->argumentChecker->checkArity(call, 1, fill ? 4 : 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *value = valueArg(call->args[0]);
    if (value->type->isError()) {
      return call;
    }
    if (fill && value->type->isInt()) {
      std::string byte = ErrorTextBufferLabel::byte();
      ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(value, type, byte);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(value->loc(), fitting.error().message);
      }
    }
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *checked = this->exprChecker->checkExpr(call->args[i]);
      if (checked->type->isString()) {
        encoding = this->bytesChecker->checkBufferEncoding(checked);
        if (i + 1 != call->args.size()) {
          std::string message = ErrorTextBuffer::encoding(name);
          this->state.getDiagnostics().error(call->args[i + 1]->loc(), message);
        }
        break;
      }
      positions.push_back(this->memberChecker->checkPosition(checked));
    }
    if (encoding && !value->type->isString()) {
      std::string message = ErrorTextBuffer::withString();
      this->state.getDiagnostics().error(encoding->loc(), message);
    }
    if (fill) {
      for (size_t i = 0; i < positions.size(); ++i) {
        std::string what = i == 0 ? ErrorTextBufferLabel::offset() : ErrorTextBufferLabel::end();
        IndexProofs::ResultUpToWasProven upTo =
            this->indexProofs->requireUpTo(positions[i], member->object, 0, what);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args = {intLit(name == "swap16" ? 2 : name == "swap32" ? 4 : 8)};
    return made(Builtin::BufferSwap, type);
  }
  NumberMethod m;
  if (!BytesCheckHelpers::numberMethod(name, false, m)) {
    return nullptr;
  }
  if (m.bigint) {
    std::string quoted = ErrorTextBigInt::quoted(name);
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(member->loc(), bigint);
    checkArgsLoosely();
    return call;
  }
  Bytes kind = m.kind;
  if (m.variable) {
    ArgumentChecker::ResultArityMatches arity =
        this->argumentChecker->checkArity(call, m.write ? 3 : 2, m.write ? 3 : 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    std::vector<Expr *> args{boolLit(m.isSigned), boolLit(m.big)};
    size_t at = 0;
    if (m.write) {
      Expr *value = this->argumentChecker->checkArg(call->args[0], Type::intTy());
      args.push_back(value);
      at = 1;
    }
    Expr *offset = this->memberChecker->checkPosition(call->args[at]);
    Expr *count = this->memberChecker->checkPosition(call->args[at + 1]);
    args.push_back(offset);
    args.push_back(count);
    std::optional<int64_t> written = this->constantFolder->intConstant(count);
    ResultCountOfBytes bytes = BufferChecker::countOfBytes(written, name);
    if (bytes.isErr()) {
      this->state.getDiagnostics().error(count->loc(), bytes.error().message);
      return call;
    }
    int64_t n = bytes.value();
    std::string offsetWhat = ErrorTextBufferLabel::offsetOf(name);
    IndexProofs::ResultUpToWasProven upTo =
        this->indexProofs->requireUpTo(offset, member->object, n, offsetWhat);
    if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(offset->loc(), upTo.error().message);
    }
    call->mayBeOutOfRange = upTo.isErr();
    if (m.write) {
      int64_t bits = n * 8;
      int64_t lo = m.isSigned ? -(int64_t(1) << (bits - 1)) : 0;
      int64_t hi = m.isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
      if (!args[2]->type->isError() && !this->byteProofs->provesFits(args[2], lo, hi)) {
        std::string range = ErrorTextBytes::range(lo, hi);
        std::string message = ErrorTextBuffer::varFits(n, range);
        this->state.getDiagnostics().error(args[2]->loc(), message);
      }
    }
    call->args = std::move(args);
    return made(m.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar, Type::intTy());
  }
  const Type *element = Type::bytesType(kind)->accessType();
  int64_t size = Type::bytesType(kind)->elementSize();
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, m.write ? 1 : 0, m.write ? 2 : 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  std::vector<Expr *> args{intLit(int64_t(kind)), boolLit(m.big)};
  size_t at = 0;
  if (m.write) {
    Expr *value = this->argumentChecker->checkArg(call->args[0], element);
    ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(
        value, Type::bytesType(kind), BytesCheckHelpers::numberWhat(kind));
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(value->loc(), fitting.error().message);
    }
    args.push_back(value);
    at = 1;
  }
  Expr *offset =
      call->args.size() > at ? this->memberChecker->checkPosition(call->args[at]) : nullptr;
  if (offset) {
    args.push_back(offset);
  }
  std::string offsetWhat = ErrorTextBufferLabel::offsetOf(name);
  IndexProofs::ResultUpToWasProven upTo =
      this->indexProofs->requireUpTo(offset, member->object, size, offsetWhat);
  if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(offset ? offset->loc() : member->object->loc(),
                                       upTo.error().message);
  }
  call->mayBeOutOfRange = upTo.isErr();
  call->args = std::move(args);

  const Type *read = element->isFloat()
                         ? Type::result(Type::doubleTy(), this->prelude->builtinType("NumberError"))
                         : element;

  return made(m.write ? Builtin::BufferWriteNumber : Builtin::BufferRead,
              m.write ? Type::intTy() : read);
}

Expr *BufferChecker::checkTextMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (type->bytes() == Bytes::TextEncoder && name == "encode") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::TextEncode};
    call->type = Type::bytesType(Bytes::Uint8);
    return call;
  }
  if (type->bytes() == Bytes::TextEncoder && name == "encodeInto") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::bytesType(Bytes::Uint8));
    call->target = BuiltinCall{.builtin = Builtin::TextEncodeInto};
    call->type = Type::object({{"read", Type::intTy(), false}, {"written", Type::intTy(), false}});
    return call;
  }
  if ((type->bytes() == Bytes::TextDecoder || type->bytes() == Bytes::FatalTextDecoder) &&
      name == "decode") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      Expr *input = call->args[0] = this->exprChecker->checkExpr(call->args[0]);
      ResultDecodedInput decoded = BufferChecker::decodedInput(input->type);
      if (decoded.isErr()) {
        this->state.getDiagnostics().error(input->loc(), decoded.error().message);
        return call;
      }
    }
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"stream", Type::optional(Type::boolTy()), false}});
      Expr *given = this->exprChecker->checkExpr(call->args[1], options);
      std::string what = ErrorTextBufferLabel::options();
      call->args[1] = this->typeRelations->coerce(given, options, what);
    }
    call->target = BuiltinCall{.builtin = Builtin::TextDecode};
    call->type = type->bytes() == Bytes::FatalTextDecoder
                     ? Type::result(Type::string(), this->prelude->builtinType("DecodeError"))
                     : Type::string();
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  std::string message = ErrorTextBuffer::noMember(name, type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

void BufferChecker::checkWriteLength(const Expr *object, const Expr *offset, const Expr *length) {
  IndexProofs &proofs = *this->indexProofs;
  std::string what = ErrorTextBufferLabel::length();
  IndexProofs::ResultBetweenProven between = proofs.requireBetween(length, 0, std::nullopt, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(length->loc(), between.error().message);
  }
  if (between.isErr() || this->indexProofs->exempt(length->loc(), object)) {
    return;
  }
  if (!proofs.provesSumUpTo(proofs.linear(offset), proofs.linear(length), object)) {
    std::string message = ErrorTextBuffer::written(BytesCheckHelpers::valueText(offset),
                                                   BytesCheckHelpers::valueText(length),
                                                   BytesCheckHelpers::valueText(object));
    this->state.getDiagnostics().error(length->loc(), message);
  }
}

}
