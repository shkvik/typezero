#include "compiler/pipeline/check_program/topics/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/functions/callback_checker/callback_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterator_methods/iterator_methods.h"
#include "compiler/pipeline/check_program/topics/json/json_static_checker/json_static_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string BufferStaticChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> BufferStaticChecker::literalText(const Expr *e) {
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

std::string BufferStaticChecker::rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

BufferStaticChecker::BufferStaticChecker(CheckState &owner)
    : state(owner) {}

Expr *BufferStaticChecker::checkBytesStatic(CallExpr *call, MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !CallChecker(this->state).isGlobalName(object, object->name)) {
    return nullptr;
  }
  if (object->name == "BigInt64Array" || object->name == "BigUint64Array") {
    DecimalChecker(this->state).reportBigInt(object->loc(), "'" + object->name.str() + "'");
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  if (type->bytes() == Bytes::ArrayBuffer && name == "isView") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    return made(Builtin::ArrayBufferIsView, Type::boolTy());
  }
  if (type->isBuffer() && name != "of") {
    return this->checkBufferStatic(call, member);
  }
  if (type->bytes() == Bytes::TextDecoder && name == "parse") {
    return BytesNewChecker(this->state).checkTextDecoderParse(call, member);
  }
  if (!type->isTypedArray()) {
    checkArgsLoosely();
    this->state.getDiagnostics().error(member->loc(),
                                       "'" + object->name + "." + name + "' does not exist");
    return call;
  }
  if (name == "of") {
    auto *lit = this->state.getAst().make<ArrayLitExpr>(call->loc());
    for (Expr *arg : call->args) {
      lit->elements.push_back({arg, false});
    }
    return BytesNewChecker(this->state).checkBytesValues(call, lit, type);
  }
  if (name != "from") {
    checkArgsLoosely();
    this->state.getDiagnostics().error(member->loc(), "'" + object->name + "." + name +
                                                          "' is not supported; of " + object->name +
                                                          " there are 'from' and 'of'");
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *&source = call->args[0];
  if (call->args.size() == 1) {
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      return BytesNewChecker(this->state).checkBytesValues(call, lit, type);
    }
    IteratorMethods(this->state).markFused(source);
    source = ExprChecker(this->state).checkExpr(source);
    if (!BytesNewChecker(this->state).checkBytesSource(source, type)) {
      return call;
    }
    return made(source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray,
                type);
  }

  return this->checkBytesFromWith(call, type, "'" + object->name.str() + ".from'");
}

Expr *BufferStaticChecker::checkBytesFromWith(CallExpr *call, const Type *type,
                                              const std::string &what) {
  Expr *&source = call->args[0];
  IteratorMethods(this->state).markFused(source);
  source = ExprChecker(this->state).checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  const Type *element = GeneratorChecker::iteratedType(from);
  if (!element || from->isString()) {
    this->state.getDiagnostics().error(
        source->loc(),
        what + " takes an array, a typed array or an iterator, found '" + from->name() + "'");
    return call;
  }
  if (from->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        GeneratorChecker(this->state).checkSendsNothing(from, what);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(source->loc(), nothing.error().message);
    }
  }
  Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  sig.ret = type->elementType();
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[1])) {
    ByteCallback info;
    if (!from->elementRange(info.lo, info.hi)) {
      info.lo = kIntMin;
      info.hi = kIntMax;
    }
    info.target = type;
    this->state.getCallbackFacts().rememberByteCallback(fe->fn, info);
  }
  bool arrow = isa<FunctionExpr>(call->args[1]);
  call->args[1] = CallbackChecker(this->state).checkFunctionArg(call->args[1], sig, 1, what);
  if (!call->args[1]->type->isFunction()) {
    return call;
  }
  int64_t lo, hi;
  if (!arrow && type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped) {
    this->state.getDiagnostics().error(
        call->args[1]->loc(),
        "cannot prove that what the function returns fits in an element of '" + type->name() +
            "' (" + BufferStaticChecker::rangeText(lo, hi) +
            "), and JavaScript would cut it without a word; pass an arrow that cuts it "
            "explicitly, e.g. 'x => " +
            wrapHint(type->bytes(), "f(x)") + "'");
    return call;
  }
  FlowChecker(this->state).calledFunction();
  auto *arrayFrom = this->state.getAst().make<CallExpr>(
      call->loc(),
      this->state.getAst().make<MemberExpr>(
          call->loc(), this->state.getAst().make<NameExpr>(call->loc(), "Array"), "from"),
      std::vector<Expr *>{source, call->args[1]});
  arrayFrom->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  arrayFrom->type = Type::array(type->elementType());
  call->args = {arrayFrom};
  call->target = BuiltinCall{.builtin = Builtin::BytesFromArray};
  call->type = type;

  return call;
}

Expr *BufferStaticChecker::checkBufferStatic(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  auto boolLit = [&](bool value) {
    auto *lit = this->state.getAst().make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto utf8Lit = [&] {
    auto *lit = this->state.getAst().make<IntLitExpr>(call->loc(), Utf8);
    lit->type = Type::intTy();

    return lit;
  };
  if (name == "from") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && call->args.size() == 1) {
      return BytesNewChecker(this->state).checkBytesValues(call, lit, buffer);
    }
    IteratorMethods(this->state).markFused(source);
    source = ExprChecker(this->state).checkExpr(source);
    const Type *from = source->type;
    if (from->isError()) {
      return call;
    }
    if (from->isString()) {
      if (call->args.size() > 2) {
        ArgumentChecker::ResultArityMatches encoded =
            ArgumentChecker(this->state).checkArity(call, 1, 2);
        if (encoded.isErr()) {
          this->state.getDiagnostics().error(member->loc(), encoded.error().message);
        }
        return call;
      }
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? BytesChecker(this->state).checkBufferEncoding(call->args[1]) : utf8Lit();
      return made(Builtin::BufferFromString, buffer);
    }
    if (from->bytes() == Bytes::ArrayBuffer) {
      for (size_t i = 1; i < call->args.size(); ++i) {
        call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
      }
      ByteProofs(this->state)
          .checkView(call->args[0], call->args.size() > 1 ? call->args[1] : nullptr,
                     call->args.size() > 2 ? call->args[2] : nullptr, 1, "the Buffer");
      return made(Builtin::BytesView, buffer);
    }
    if (call->args.size() > 1) {
      this->state.getDiagnostics().error(call->args[1]->loc(),
                                         "'Buffer.from' takes an offset and a length only with "
                                         "an ArrayBuffer, found '" +
                                             from->name() + "'");
      return call;
    }
    if (!BytesNewChecker(this->state).checkBytesSource(source, buffer)) {
      return call;
    }
    return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, buffer);
  }
  if (name == "alloc") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state)
            .requireBetween(call->args[0], 0, std::nullopt, "the size of a Buffer");
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    if (call->args.size() == 1) {
      call->args.push_back(boolLit(true));
      return made(Builtin::BytesNew, buffer);
    }
    Expr *fill = ExprChecker(this->state).checkExpr(call->args[1]);
    const Type *t = fill->type;
    if (t->isError()) {
      return call;
    }
    if (t->isInteger()) {
      fill = TypeRelations(this->state).convert(fill, Type::intTy() == t ? t : Type::intTy());
      ByteProofs(this->state).checkFits(fill, buffer, "a byte");
    } else if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      fill = TypeRelations(this->state).convert(fill, uint8);
    } else if (!t->isString()) {
      this->state.getDiagnostics().error(
          fill->loc(), "'Buffer.alloc' fills with a number, a string or a Uint8Array, "
                       "found '" +
                           t->name() + "'");
      return call;
    }
    Expr *encoding = utf8Lit();
    if (call->args.size() == 3) {
      encoding = BytesChecker(this->state).checkBufferEncoding(call->args[2]);
      if (!t->isString()) {
        this->state.getDiagnostics().error(encoding->loc(),
                                           "an encoding goes only with a string to fill with");
      }
    }
    call->args = {call->args[0], fill, encoding};
    return made(Builtin::BufferAllocFill, buffer);
  }
  if (name == "allocUnsafe" || name == "allocUnsafeSlow") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state)
            .requireBetween(call->args[0], 0, std::nullopt, "the size of a Buffer");
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    call->args.push_back(boolLit(false));
    return made(Builtin::BytesNew, buffer);
  }
  if (name == "concat") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&list = call->args[0];
    list = ExprChecker(this->state).checkExpr(list, Type::array(uint8));
    const Type *t = list->type;
    if (t->isError()) {
      return call;
    }
    if (!t->isArray() || (t->inner() != uint8 && t->inner() != buffer)) {
      this->state.getDiagnostics().error(
          list->loc(), "'Buffer.concat' takes an array of Uint8Arrays or Buffers, "
                       "found '" +
                           t->name() + "'");
      return call;
    }
    if (call->args.size() == 2) {
      call->args[1] = MemberChecker(this->state).checkPosition(call->args[1]);
      IndexProofs::ResultBetweenProven between =
          IndexProofs(this->state)
              .requireBetween(call->args[1], 0, std::nullopt, "the total length");
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(call->args[1]->loc(), between.error().message);
      }
    }
    return made(Builtin::BufferConcat, buffer);
  }
  if (name == "byteLength") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    const Type *t = call->args[0]->type;
    if (t->isError()) {
      return call;
    }
    if (t->isString()) {
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? BytesChecker(this->state).checkBufferEncoding(call->args[1]) : utf8Lit();
    } else if (!t->isByteView() && t->bytes() != Bytes::ArrayBuffer) {
      this->state.getDiagnostics().error(call->args[0]->loc(),
                                         "'Buffer.byteLength' takes a string, a typed array, a "
                                         "DataView or an ArrayBuffer, found '" +
                                             t->name() + "'");
      return call;
    }
    return made(Builtin::BufferByteLength, Type::intTy());
  }
  if (name == "compare") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = ArgumentChecker(this->state).checkArg(arg, uint8);
    }
    return made(Builtin::BufferCompare, Type::intTy());
  }
  if (name == "isBuffer") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    const Type *arg = call->args[0]->type;
    const Type *base = arg->nonOptional();
    auto mayBe = [&](const Type *t) { return t->isBuffer() || t->bytes() == Bytes::Uint8; };
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    if (!arg->isError() && llvm::any_of(members, mayBe) &&
        (arg->isOptional() ||
         !llvm::all_of(members, [](const Type *t) { return t->isBuffer(); }))) {
      auto *rhs = this->state.getAst().make<NameExpr>(member->loc(), "Buffer");
      rhs->type = Type::bytesType(Bytes::Buffer);
      auto *test = this->state.getAst().make<BinaryExpr>(call->loc(), BinaryOp::InstanceOf,
                                                         call->args[0], rhs);
      test->type = Type::boolTy();
      return test;
    }
    return made(Builtin::BufferIsBuffer, Type::boolTy());
  }
  if (name == "isEncoding") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (std::optional<std::u16string> text = BufferStaticChecker::literalText(call->args[0])) {
      return boolLit(encodingNamed(BufferStaticChecker::utf8(*text)).has_value());
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    return made(Builtin::BufferIsEncoding, Type::boolTy());
  }
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
  this->state.getDiagnostics().error(
      member->loc(), "'Buffer." + name +
                         "' is not supported; Buffer has from, alloc, "
                         "allocUnsafe, concat, byteLength, compare, isBuffer, isEncoding "
                         "and of");

  return call;
}

Expr *BufferStaticChecker::checkEmbedBytes(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
    return call;
  }
  std::string bytes, shown, full;
  if (!JsonStaticChecker(this->state).readEmbedded(call, "embedBytes", bytes, shown, full)) {
    return call;
  }
  if (int64_t(bytes.size()) > kMaxLength) {
    this->state.getDiagnostics().error(call->args[0]->loc(),
                                       "'" + shown +
                                           "' is larger than the 1 GiB a Uint8Array "
                                           "may have");
    return call;
  }
  llvm::SMLoc loc = call->args[0]->loc();
  std::u16string units(bytes.size(), u'\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    units[i] = char16_t(static_cast<unsigned char>(bytes[i]));
  }
  auto *lit = this->state.getAst().make<StringLitExpr>(loc, std::move(units));
  lit->type = Type::string();
  llvm::SmallVector<llvm::UTF16, 64> path;
  llvm::convertUTF8ToUTF16String(full, path);
  auto *file =
      this->state.getAst().make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {lit, file};
  call->target = BuiltinCall{.builtin = Builtin::EmbedBytes};
  call->type = Type::bytesType(Bytes::Uint8);

  return call;
}

}
