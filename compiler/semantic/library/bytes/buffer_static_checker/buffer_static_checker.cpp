#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/bytes/buffer_static_table/buffer_static_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
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
  this->byteProofs = &all.byteProofs;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->bytesChecker = &all.bytesChecker;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->bytesNewChecker = &all.bytesNewChecker;
  this->callbackChecker = &all.callbackChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
  this->jsonStaticChecker = &all.jsonStaticChecker;
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
  bool isGlobal = object != nullptr && this->callChecker->isUnshadowedGlobal(object);
  if (!isGlobal) {
    return nullptr;
  }
  std::optional<GlobalName> globalName = GlobalNameTable::find(object->name);
  bool isBigint =
      globalName == GlobalName::BIG_INT64_ARRAY || globalName == GlobalName::BIG_UINT64_ARRAY;
  if (isBigint) {
    return this->reportBigIntArray(call, object);
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (type == nullptr) {
    return nullptr;
  }
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }

  StaticCall bytes{
      .call = call,
      .member = member,
      .object = object,
      .type = type,
  };
  std::optional<BytesMethod> method = BytesMethodTable::findMethod(member->name);
  if (type->bytes() == Bytes::ArrayBuffer && method == BytesMethod::IS_VIEW) {
    return this->checkIsView(bytes);
  }
  if (type->isBuffer() && method != BytesMethod::OF) {
    return this->checkBufferStatic(call, member);
  }
  if (type->bytes() == Bytes::TextDecoder && method == BytesMethod::PARSE) {
    return this->bytesNewChecker->checkTextDecoderParse(call, member);
  }
  if (!type->isTypedArray()) {
    std::string message = ErrorTextBytesStatic::missing(object->name, member->name);
    return this->reportStatic(call, member, message);
  }
  if (method == BytesMethod::OF) {
    return this->checkTypedArrayOf(bytes);
  }
  if (method != BytesMethod::FROM) {
    std::string message = ErrorTextBytesStatic::notYet(object->name, member->name);
    return this->reportStatic(call, member, message);
  }

  return this->checkTypedArrayFrom(bytes);
}

Expr *BufferStaticChecker::reportBigIntArray(CallExpr *call, NameExpr *object) {
  std::string quoted = ErrorTextBigInt::quoted(object->name);
  std::string bigint = ErrorTextBigInt::none(quoted);
  this->state.getDiagnostics().error(object->loc(), bigint);
  this->argumentChecker->walkArgsLoosely(call);
  call->type = Type::error();

  return call;
}

Expr *BufferStaticChecker::reportStatic(CallExpr *call, MemberExpr *member,
                                        const std::string &message) {
  this->argumentChecker->walkArgsLoosely(call);
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *BufferStaticChecker::checkIsView(const StaticCall &bytes) {
  CallExpr *call = bytes.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(bytes.member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);

  return BufferStaticChecker::toBuiltinCall(call, Builtin::ArrayBufferIsView, Type::boolTy());
}

Expr *BufferStaticChecker::checkTypedArrayOf(const StaticCall &bytes) {
  CallExpr *call = bytes.call;
  auto *literal = this->state.getAst().make<ArrayLitExpr>(call->loc());
  for (Expr *arg : call->args) {
    literal->elements.push_back({arg, false});
  }

  return this->bytesNewChecker->checkBytesValues(call, literal, bytes.type);
}

Expr *BufferStaticChecker::checkTypedArrayFrom(const StaticCall &bytes) {
  CallExpr *call = bytes.call;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(bytes.member->loc(), arity.error().message);
    return call;
  }
  if (call->args.size() != 1) {
    std::string what = ErrorTextBytesStatic::from(bytes.object->name);
    return this->checkBytesFromWith(call, bytes.type, what);
  }
  Expr *&source = call->args[0];
  auto *literal = dyn_cast<ArrayLitExpr>(source);
  if (literal != nullptr) {
    return this->bytesNewChecker->checkBytesValues(call, literal, bytes.type);
  }
  this->iteratorMethods->markFused(source);
  source = this->exprChecker->checkExpr(source);
  if (!this->bytesNewChecker->checkBytesSource(source, bytes.type)) {
    return call;
  }
  Builtin builtin =
      source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return BufferStaticChecker::toBuiltinCall(call, builtin, bytes.type);
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
  if (from->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->generatorChecker->checkSendsNothing(from, what);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(source->loc(), nothing.error().message);
    }
  }

  Signature signature = BufferStaticChecker::mapperSignature(iterated.value(), type);
  this->rememberByteCallback(call->args[1], from, type);
  bool isArrow = isa<FunctionExpr>(call->args[1]);
  call->args[1] = this->callbackChecker->checkFunctionArg(call->args[1], signature, 1, what);
  if (!call->args[1]->type->isFunction()) {
    return call;
  }
  ResultMapResultFits fits = BufferStaticChecker::mapResultFits(type, isArrow);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), fits.error().message);
    return call;
  }
  this->flowChecker->calledFunction();
  CallExpr *arrayFrom = this->makeArrayFrom(call, source, type);
  call->args = {arrayFrom};

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BytesFromArray, type);
}

Signature BufferStaticChecker::mapperSignature(const Type *element, const Type *type) {
  Signature signature;
  signature.pad = false;
  signature.params = {element, Type::intTy()};
  signature.ret = type->elementType();

  return signature;
}

void BufferStaticChecker::rememberByteCallback(const Expr *arg, const Type *from,
                                               const Type *type) {
  auto *function = dyn_cast<FunctionExpr>(arg);
  if (function != nullptr) {
    ByteCallback info;
    if (!from->elementRange(info.lo, info.hi)) {
      info.lo = kIntMin;
      info.hi = kIntMax;
    }
    info.target = type;
    this->state.getCallbackFacts().rememberByteCallback(function->fn, info);
  }
}

BufferStaticChecker::ResultMapResultFits BufferStaticChecker::mapResultFits(const Type *type,
                                                                            bool arrow) {
  int64_t lo = 0;
  int64_t hi = 0;
  bool isBounded = !arrow && type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped;
  if (isBounded) {
    std::string elementWhat = ErrorTextByteView::element(type->name());
    std::string range = ErrorTextBytes::range(lo, hi);
    std::string hint = BytesCheckHelpers::wrapHint(type->bytes(), "f(x)");
    std::string message = ErrorTextTypedArray::mapFits(elementWhat, range, hint);

    return err(BufferStaticCheckerMapResultFitsError{
        .code = BufferStaticCheckerMapResultFitsErrorCode::RESULT_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(type);
}

CallExpr *BufferStaticChecker::makeArrayFrom(CallExpr *call, Expr *source, const Type *type) {
  llvm::StringRef arrayGlobal = GlobalNameTable::getName(GlobalName::ARRAY);
  llvm::StringRef fromName = GlobalMemberTable::getName(GlobalMember::ARRAY_FROM);
  auto *arrayName = this->state.getAst().make<NameExpr>(call->loc(), arrayGlobal);
  auto *callee = this->state.getAst().make<MemberExpr>(call->loc(), arrayName, fromName);
  std::vector<Expr *> args{source, call->args[1]};
  auto *arrayFrom = this->state.getAst().make<CallExpr>(call->loc(), callee, args);
  arrayFrom->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  arrayFrom->type = Type::array(type->elementType());

  return arrayFrom;
}

Expr *BufferStaticChecker::checkBufferStatic(CallExpr *call, MemberExpr *member) {
  std::optional<BytesMethod> method = BytesMethodTable::findMethod(member->name);
  const BufferStaticInfo *found = method.has_value() ? BufferStaticTable::find(*method) : nullptr;
  if (found == nullptr) {
    std::string message = ErrorTextBufferStatic::unknown(member->name);
    return this->reportStatic(call, member, message);
  }

  switch (found->kind) {
    case BufferStaticKind::FROM:
      return this->checkBufferFrom(call, member);
    case BufferStaticKind::ALLOC:
      return this->checkAlloc(call, member);
    case BufferStaticKind::ALLOC_UNSAFE:
      return this->checkAllocUnsafe(call, member);
    case BufferStaticKind::CONCAT:
      return this->checkConcat(call, member);
    case BufferStaticKind::BYTE_LENGTH:
      return this->checkByteLength(call, member);
    case BufferStaticKind::COMPARE:
      return this->checkBufferCompare(call, member);
    case BufferStaticKind::IS_BUFFER:
      return this->checkIsBuffer(call, member);
    case BufferStaticKind::IS_ENCODING:
      return this->checkIsEncoding(call, member);
  }

  return call;
}

Expr *BufferStaticChecker::checkBufferFrom(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *&source = call->args[0];
  auto *literal = dyn_cast<ArrayLitExpr>(source);
  if (literal != nullptr && call->args.size() == 1) {
    return this->bytesNewChecker->checkBytesValues(call, literal, buffer);
  }
  this->iteratorMethods->markFused(source);
  source = this->exprChecker->checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  if (from->isString()) {
    return this->checkBufferFromString(call, member);
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    return this->checkBufferView(call);
  }
  if (call->args.size() > 1) {
    std::string message = ErrorTextBufferStatic::offsetOnly(from->name());
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return call;
  }
  if (!this->bytesNewChecker->checkBytesSource(source, buffer)) {
    return call;
  }
  Builtin builtin = from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return BufferStaticChecker::toBuiltinCall(call, builtin, buffer);
}

Expr *BufferStaticChecker::checkBufferFromString(CallExpr *call, MemberExpr *member) {
  if (call->args.size() > 2) {
    ArgumentChecker::ResultArityMatches encoded = this->argumentChecker->checkArity(call, 1, 2);
    if (encoded.isErr()) {
      this->state.getDiagnostics().error(member->loc(), encoded.error().message);
    }
    return call;
  }
  call->args.resize(2, nullptr);
  call->args[1] = this->encodingOrUtf8(call, call->args[1]);
  const Type *buffer = Type::bytesType(Bytes::Buffer);

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferFromString, buffer);
}

Expr *BufferStaticChecker::checkBufferView(CallExpr *call) {
  for (size_t i = 1; i < call->args.size(); ++i) {
    call->args[i] = this->memberChecker->checkPosition(call->args[i]);
  }
  std::string what = ErrorTextBufferStatic::buffer();
  Expr *offset = call->args.size() > 1 ? call->args[1] : nullptr;
  Expr *length = call->args.size() > 2 ? call->args[2] : nullptr;
  this->byteProofs->walkView(call->args[0], offset, length, 1, what);
  const Type *buffer = Type::bytesType(Bytes::Buffer);

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BytesView, buffer);
}

Expr *BufferStaticChecker::checkAlloc(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  this->walkAllocSize(call);
  if (call->args.size() == 1) {
    call->args.push_back(this->makeBoolLiteral(call->loc(), true));
    return BufferStaticChecker::toBuiltinCall(call, Builtin::BytesNew, buffer);
  }

  Expr *fill = this->exprChecker->checkExpr(call->args[1]);
  const Type *fillType = fill->type;
  if (fillType->isError()) {
    return call;
  }
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  if (fillType->isInteger()) {
    fill = this->convertByteFill(fill);
  } else if (fillType->isTypedArray() && TypeRelations::isImplicitlyConvertible(fillType, uint8)) {
    fill = this->typeRelations->convert(fill, uint8);
  } else if (!fillType->isString()) {
    std::string message = ErrorTextBufferStatic::allocFill(fillType->name());
    this->state.getDiagnostics().error(fill->loc(), message);
    return call;
  }
  Expr *encoding = this->allocEncoding(call, fillType);
  call->args = {call->args[0], fill, encoding};

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferAllocFill, buffer);
}

Expr *BufferStaticChecker::convertByteFill(Expr *fill) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  Expr *converted = this->typeRelations->convert(fill, Type::intTy());
  std::string byte = ErrorTextBufferLabel::byte();
  ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(converted, buffer, byte);
  if (fitting.isErr()) {
    this->state.getDiagnostics().error(converted->loc(), fitting.error().message);
  }

  return converted;
}

Expr *BufferStaticChecker::allocEncoding(CallExpr *call, const Type *fillType) {
  if (call->args.size() != 3) {
    return this->makeUtf8Literal(call->loc());
  }
  Expr *encoding = this->bytesChecker->checkBufferEncoding(call->args[2]);
  if (!fillType->isString()) {
    std::string message = ErrorTextBufferStatic::fillCoding();
    this->state.getDiagnostics().error(encoding->loc(), message);
  }

  return encoding;
}

Expr *BufferStaticChecker::checkAllocUnsafe(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  this->walkAllocSize(call);
  call->args.push_back(this->makeBoolLiteral(call->loc(), false));
  const Type *buffer = Type::bytesType(Bytes::Buffer);

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BytesNew, buffer);
}

void BufferStaticChecker::walkAllocSize(CallExpr *call) {
  call->args[0] = this->memberChecker->checkPosition(call->args[0]);
  std::string size = ErrorTextBufferStatic::size();
  IndexProofs::ResultBetweenProven between =
      this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, size);
  this->reportUnprovenBetween(between, call->args[0]->loc());
}

Expr *BufferStaticChecker::checkConcat(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *&list = call->args[0];
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
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
    this->reportUnprovenBetween(between, call->args[1]->loc());
  }

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferConcat, buffer);
}

Expr *BufferStaticChecker::checkByteLength(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *measured = call->args[0]->type;
  if (measured->isError()) {
    return call;
  }
  bool hasBytes = measured->isByteView() || measured->bytes() == Bytes::ArrayBuffer;
  if (measured->isString()) {
    call->args.resize(2, nullptr);
    call->args[1] = this->encodingOrUtf8(call, call->args[1]);
  } else if (!hasBytes) {
    std::string message = ErrorTextBufferStatic::byteLength(measured->name());
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferByteLength, Type::intTy());
}

Expr *BufferStaticChecker::checkBufferCompare(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  for (Expr *&arg : call->args) {
    arg = this->argumentChecker->checkArg(arg, uint8);
  }

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferCompare, Type::intTy());
}

Expr *BufferStaticChecker::checkIsBuffer(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *tested = call->args[0]->type;
  const Type *base = tested->nonOptional();
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  bool isUndecided =
      !tested->isError() && llvm::any_of(members, BufferStaticChecker::mayBeBuffer) &&
      (tested->isOptional() || !llvm::all_of(members, BufferStaticChecker::isBuffer));
  if (isUndecided) {
    return this->makeBufferTest(call, member);
  }

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferIsBuffer, Type::boolTy());
}

bool BufferStaticChecker::mayBeBuffer(const Type *type) {
  return type->isBuffer() || type->bytes() == Bytes::Uint8;
}

bool BufferStaticChecker::isBuffer(const Type *type) {
  return type->isBuffer();
}

Expr *BufferStaticChecker::makeBufferTest(CallExpr *call, MemberExpr *member) {
  llvm::StringRef bufferName = GlobalNameTable::getName(GlobalName::BUFFER);
  auto *rhs = this->state.getAst().make<NameExpr>(member->loc(), bufferName);
  rhs->type = Type::bytesType(Bytes::Buffer);
  auto *test =
      this->state.getAst().make<BinaryExpr>(call->loc(), BinaryOp::InstanceOf, call->args[0], rhs);
  test->type = Type::boolTy();

  return test;
}

Expr *BufferStaticChecker::checkIsEncoding(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  std::optional<std::u16string> text = BufferStaticChecker::literalText(call->args[0]);
  if (text.has_value()) {
    std::string utf8 = ToUtf8::toUtf8(*text);
    bool isKnown = BytesCheckHelpers::encodingNamed(utf8).has_value();
    return this->makeBoolLiteral(call->loc(), isKnown);
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());

  return BufferStaticChecker::toBuiltinCall(call, Builtin::BufferIsEncoding, Type::boolTy());
}

Expr *BufferStaticChecker::encodingOrUtf8(CallExpr *call, Expr *encoding) {
  if (encoding != nullptr) {
    return this->bytesChecker->checkBufferEncoding(encoding);
  }

  return this->makeUtf8Literal(call->loc());
}

Expr *BufferStaticChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

void BufferStaticChecker::reportUnprovenBetween(const IndexProofs::ResultBetweenProven &between,
                                                llvm::SMLoc at) {
  bool isShown =
      between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.getDiagnostics().error(at, between.error().message);
  }
}

BoolLitExpr *BufferStaticChecker::makeBoolLiteral(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.getAst().make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

IntLitExpr *BufferStaticChecker::makeUtf8Literal(llvm::SMLoc loc) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, Utf8);
  literal->type = Type::intTy();

  return literal;
}

Expr *BufferStaticChecker::checkEmbedBytes(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
    return call;
  }
  std::string bytes, shown, full;
  llvm::StringRef fn = GlobalNameTable::getName(GlobalName::EMBED_BYTES);
  if (!this->jsonStaticChecker->readEmbedded(call, fn, bytes, shown, full)) {
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
