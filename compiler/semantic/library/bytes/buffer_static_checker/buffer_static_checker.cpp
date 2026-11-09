#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker_errors.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker_errors.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/json/json_static_checker/json_static_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

BufferStaticChecker::BufferStaticChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.getByteProofs();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->bytesChecker = &all.getBytesChecker();
  this->memberChecker = &all.getMemberChecker();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->bytesNewChecker = &all.getBytesNewChecker();
  this->callbackChecker = &all.getCallbackChecker();
  this->iteratorMethods = &all.getIteratorMethods();
  this->generatorChecker = &all.getGeneratorChecker();
  this->jsonStaticChecker = &all.getJsonStaticChecker();
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

BufferStaticChecker::ResultSourceElement BufferStaticChecker::sourceElement(const Type *from,
                                                                            llvm::StringRef what) {
  const Type *element = GeneratorChecker::iteratedType(from);
  if (!element || from->isString()) {
    std::string message = ErrorTextBytesStatic::source(what, from->name());

    return err(BufferStaticCheckerSourceElementError{
        .code = BufferStaticCheckerSourceElementErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(element);
}

BufferStaticChecker::ResultConcatSources BufferStaticChecker::concatSources(const Type *list,
                                                                            const Type *buffer) {
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  if (!list->isArray() || (list->inner() != uint8 && list->inner() != buffer)) {
    std::string message = ErrorTextBufferStatic::concat(list->name());

    return err(BufferStaticCheckerConcatSourcesError{
        .code = BufferStaticCheckerConcatSourcesErrorCode::NOT_BYTES_LIST,
        .message = message,
    });
  }

  return ok(list);
}

Expr *BufferStaticChecker::checkBytesStatic(CallExpr *call, MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !this->callChecker->isGlobalName(object, object->name)) {
    return nullptr;
  }
  if (object->name == "BigInt64Array" || object->name == "BigUint64Array") {
    std::string quoted = ErrorTextBigInt::quoted(object->name);
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(object->loc(), bigint);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  if (type->bytes() == Bytes::ArrayBuffer && name == "isView") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    return made(Builtin::ArrayBufferIsView, Type::boolTy());
  }
  if (type->isBuffer() && name != "of") {
    return this->checkBufferStatic(call, member);
  }
  if (type->bytes() == Bytes::TextDecoder && name == "parse") {
    return this->bytesNewChecker->checkTextDecoderParse(call, member);
  }
  if (!type->isTypedArray()) {
    checkArgsLoosely();
    std::string message = ErrorTextBytesStatic::missing(object->name, name);
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  if (name == "of") {
    auto *lit = this->state.getAst().make<ArrayLitExpr>(call->loc());
    for (Expr *arg : call->args) {
      lit->elements.push_back({arg, false});
    }
    return this->bytesNewChecker->checkBytesValues(call, lit, type);
  }
  if (name != "from") {
    checkArgsLoosely();
    std::string message = ErrorTextBytesStatic::notYet(object->name, name);
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *&source = call->args[0];
  if (call->args.size() == 1) {
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      return this->bytesNewChecker->checkBytesValues(call, lit, type);
    }
    this->iteratorMethods->markFused(source);
    source = this->exprChecker->checkExpr(source);
    if (!this->bytesNewChecker->checkBytesSource(source, type)) {
      return call;
    }
    return made(source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray,
                type);
  }

  std::string what = ErrorTextBytesStatic::from(object->name);

  return this->checkBytesFromWith(call, type, what);
}

Expr *BufferStaticChecker::checkBytesFromWith(CallExpr *call, const Type *type,
                                              const std::string &what) {
  Expr *&source = call->args[0];
  this->iteratorMethods->markFused(source);
  source = this->exprChecker->checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  ResultSourceElement iterated = BufferStaticChecker::sourceElement(from, what);
  if (iterated.isErr()) {
    this->state.getDiagnostics().error(source->loc(), iterated.error().message);
    return call;
  }
  const Type *element = iterated.value();
  if (from->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->generatorChecker->checkSendsNothing(from, what);
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
  call->args[1] = this->callbackChecker->checkFunctionArg(call->args[1], sig, 1, what);
  if (!call->args[1]->type->isFunction()) {
    return call;
  }
  int64_t lo, hi;
  if (!arrow && type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped) {
    std::string elementWhat = ErrorTextByteView::element(type->name());
    std::string range = ErrorTextBytes::range(lo, hi);
    std::string hint = BytesCheckHelpers::wrapHint(type->bytes(), "f(x)");
    std::string message = ErrorTextTypedArray::mapFits(elementWhat, range, hint);
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return call;
  }
  this->flowChecker->calledFunction();
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && call->args.size() == 1) {
      return this->bytesNewChecker->checkBytesValues(call, lit, buffer);
    }
    this->iteratorMethods->markFused(source);
    source = this->exprChecker->checkExpr(source);
    const Type *from = source->type;
    if (from->isError()) {
      return call;
    }
    if (from->isString()) {
      if (call->args.size() > 2) {
        ArgumentChecker::ResultArityMatches encoded = this->argumentChecker->checkArity(call, 1, 2);
        if (encoded.isErr()) {
          this->state.getDiagnostics().error(member->loc(), encoded.error().message);
        }
        return call;
      }
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? this->bytesChecker->checkBufferEncoding(call->args[1]) : utf8Lit();
      return made(Builtin::BufferFromString, buffer);
    }
    if (from->bytes() == Bytes::ArrayBuffer) {
      for (size_t i = 1; i < call->args.size(); ++i) {
        call->args[i] = this->memberChecker->checkPosition(call->args[i]);
      }
      std::string what = ErrorTextBufferStatic::buffer();
      this->byteProofs->checkView(call->args[0], call->args.size() > 1 ? call->args[1] : nullptr,
                                  call->args.size() > 2 ? call->args[2] : nullptr, 1, what);
      return made(Builtin::BytesView, buffer);
    }
    if (call->args.size() > 1) {
      std::string message = ErrorTextBufferStatic::offsetOnly(from->name());
      this->state.getDiagnostics().error(call->args[1]->loc(), message);
      return call;
    }
    if (!this->bytesNewChecker->checkBytesSource(source, buffer)) {
      return call;
    }
    return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, buffer);
  }
  if (name == "alloc") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->memberChecker->checkPosition(call->args[0]);
    std::string size = ErrorTextBufferStatic::size();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, size);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    if (call->args.size() == 1) {
      call->args.push_back(boolLit(true));
      return made(Builtin::BytesNew, buffer);
    }
    Expr *fill = this->exprChecker->checkExpr(call->args[1]);
    const Type *t = fill->type;
    if (t->isError()) {
      return call;
    }
    if (t->isInteger()) {
      fill = this->typeRelations->convert(fill, Type::intTy() == t ? t : Type::intTy());
      std::string byte = ErrorTextBufferLabel::byte();
      ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(fill, buffer, byte);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(fill->loc(), fitting.error().message);
      }
    } else if (t->isTypedArray() && TypeRelations::isImplicitlyConvertible(t, uint8)) {
      fill = this->typeRelations->convert(fill, uint8);
    } else if (!t->isString()) {
      std::string message = ErrorTextBufferStatic::allocFill(t->name());
      this->state.getDiagnostics().error(fill->loc(), message);
      return call;
    }
    Expr *encoding = utf8Lit();
    if (call->args.size() == 3) {
      encoding = this->bytesChecker->checkBufferEncoding(call->args[2]);
      if (!t->isString()) {
        std::string message = ErrorTextBufferStatic::fillCoding();
        this->state.getDiagnostics().error(encoding->loc(), message);
      }
    }
    call->args = {call->args[0], fill, encoding};
    return made(Builtin::BufferAllocFill, buffer);
  }
  if (name == "allocUnsafe" || name == "allocUnsafeSlow") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->memberChecker->checkPosition(call->args[0]);
    std::string size = ErrorTextBufferStatic::size();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, size);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    call->args.push_back(boolLit(false));
    return made(Builtin::BytesNew, buffer);
  }
  if (name == "concat") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&list = call->args[0];
    list = this->exprChecker->checkExpr(list, Type::array(uint8));
    if (list->type->isError()) {
      return call;
    }
    ResultConcatSources sources = BufferStaticChecker::concatSources(list->type, buffer);
    if (sources.isErr()) {
      this->state.getDiagnostics().error(list->loc(), sources.error().message);
      return call;
    }
    if (call->args.size() == 2) {
      call->args[1] = this->memberChecker->checkPosition(call->args[1]);
      std::string total = ErrorTextBufferStatic::total();
      IndexProofs::ResultBetweenProven between =
          this->indexProofs->requireBetween(call->args[1], 0, std::nullopt, total);
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(call->args[1]->loc(), between.error().message);
      }
    }
    return made(Builtin::BufferConcat, buffer);
  }
  if (name == "byteLength") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    const Type *t = call->args[0]->type;
    if (t->isError()) {
      return call;
    }
    if (t->isString()) {
      call->args.resize(2, nullptr);
      call->args[1] =
          call->args[1] ? this->bytesChecker->checkBufferEncoding(call->args[1]) : utf8Lit();
    } else if (!t->isByteView() && t->bytes() != Bytes::ArrayBuffer) {
      std::string message = ErrorTextBufferStatic::byteLength(t->name());
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
      return call;
    }
    return made(Builtin::BufferByteLength, Type::intTy());
  }
  if (name == "compare") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = this->argumentChecker->checkArg(arg, uint8);
    }
    return made(Builtin::BufferCompare, Type::intTy());
  }
  if (name == "isBuffer") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (std::optional<std::u16string> text = BufferStaticChecker::literalText(call->args[0])) {
      return boolLit(BytesCheckHelpers::encodingNamed(ToUtf8::toUtf8(*text)).has_value());
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    return made(Builtin::BufferIsEncoding, Type::boolTy());
  }
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  std::string message = ErrorTextBufferStatic::unknown(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *BufferStaticChecker::checkEmbedBytes(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
    return call;
  }
  std::string bytes, shown, full;
  if (!this->jsonStaticChecker->readEmbedded(call, "embedBytes", bytes, shown, full)) {
    return call;
  }
  if (int64_t(bytes.size()) > Type::kMaxLength) {
    std::string message = ErrorTextBufferStatic::embedSize(shown);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
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
