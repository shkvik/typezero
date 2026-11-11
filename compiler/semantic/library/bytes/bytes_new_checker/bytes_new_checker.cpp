#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

BytesNewChecker::BytesNewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->byteProofs = &all.byteProofs;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->literalTypes = &all.literalTypes;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

std::optional<std::u16string> BytesNewChecker::literalText(const Expr *e) {
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

bool BytesNewChecker::mayBeFatal(const Expr *options) {
  std::vector<bool> answers;
  for (const Expr *node : ChildrenFirst::order(options, BytesNewChecker::spreadOptions)) {
    bool isFatal = BytesNewChecker::nodeMayBeFatal(node, answers);
    answers.push_back(isFatal);
  }

  return answers.back();
}

NodeChildren<const Expr *> BytesNewChecker::spreadOptions(const Expr *options) {
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  if (options->type->isError() || literal == nullptr) {
    return {};
  }
  NodeChildren<const Expr *> spreads;
  for (const ObjectLitExpr::Property &prop : literal->props) {
    if (prop.spread()) {
      spreads.push_back(prop.value);
    }
  }

  return spreads;
}

bool BytesNewChecker::nodeMayBeFatal(const Expr *options, std::vector<bool> &answers) {
  if (options->type->isError()) {
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  if (!literal) {
    const Field *field = options->type->field("fatal");
    return field && !field->type->isUndefined();
  }
  size_t spreadCount = BytesNewChecker::spreadOptions(options).size();
  std::vector<bool> spreads = ChildrenFirst::takeAnswers(answers, spreadCount);
  size_t nextSpread = 0;
  bool isFatal = false;
  for (const ObjectLitExpr::Property &prop : literal->props) {
    if (prop.spread()) {
      isFatal = isFatal || spreads[nextSpread];
      nextSpread += 1;
    } else if (prop.name == "fatal") {
      isFatal = BytesNewChecker::fatalFlag(prop.value);
    }
  }

  return isFatal;
}

bool BytesNewChecker::fatalFlag(const Expr *value) {
  const Expr *plain = value;
  while (auto *conv = dyn_cast<CastExpr>(plain)) {
    if (!conv->isImplicit()) {
      break;
    }
    plain = conv->operand;
  }
  auto *flag = dyn_cast<BoolLitExpr>(plain);

  return !isa<UndefinedExpr>(plain) && !(flag && !flag->value);
}

BytesNewChecker::ResultDecoderLabel BytesNewChecker::decoderLabel(llvm::StringRef label) {
  std::string lower = label.trim(" \t\n\f\r").lower();
  const char *found = tz_encoding_name(lower.c_str());
  if (!found) {
    std::string message = ErrorTextTextDecoder::unknown(label);

    return err(BytesNewCheckerDecoderLabelError{
        .code = BytesNewCheckerDecoderLabelErrorCode::UNKNOWN_LABEL,
        .message = message,
    });
  }

  return ok(found);
}

BytesNewChecker::ResultViewedBuffer BytesNewChecker::viewedBuffer(const Type *buffer) {
  if (buffer->bytes() != Bytes::ArrayBuffer && !buffer->isError()) {
    std::string message = ErrorTextBytesNew::viewOver(buffer->name(), buffer->isTypedArray());

    return err(BytesNewCheckerViewedBufferError{
        .code = BytesNewCheckerViewedBufferErrorCode::NOT_A_BUFFER,
        .message = message,
    });
  }

  return ok(buffer);
}

Expr *BytesNewChecker::checkTextDecoderParse(CallExpr *call, MemberExpr *member) {
  auto *expr = this->state.getAst().make<NewExpr>(call->loc(), "TextDecoder",
                                                  std::vector<TypeRef>{}, call->args);
  BytesStore &bytes = this->state.getBytesStore();
  bytes.enterDecoderParse();
  Expr *made = this->checkBytesNew(expr, Type::bytesType(Bytes::TextDecoder));
  bytes.leaveDecoderParse();
  auto *result = dyn_cast<CallExpr>(made);
  if (!result || result->builtin() != Builtin::TextDecoderNew) {
    return made;
  }
  result->callee = member;
  result->target = BuiltinCall{.builtin = Builtin::TextDecoderParse};
  result->type = Type::result(result->type, this->prelude->builtinType("EncodingError"));

  return result;
}

Expr *BytesNewChecker::checkBytesNew(NewExpr *expr, const Type *type) {
  llvm::SMLoc loc = expr->loc();
  auto *callee = this->state.getAst().make<NameExpr>(loc, type->name());
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, expr->args);
  if (!expr->typeArgs.empty()) {
    std::string message = ErrorTextBytesNew::typeArgs(type->name());
    this->state.getDiagnostics().error(loc, message);
  }
  call->type = Type::error();
  switch (type->bytes()) {
    case Bytes::Buffer:
      return this->reportBytesNew(expr, ErrorTextBytesNew::buffer());
    case Bytes::ArrayBuffer:
      return this->checkArrayBufferNew(expr, call, type);
    case Bytes::DataView:
      return this->checkDataViewNew(call, type);
    case Bytes::TextEncoder:
      return this->checkTextEncoderNew(call, type);
    case Bytes::TextDecoder:
      return this->checkTextDecoderNew(call, type);
    case Bytes::FatalTextDecoder:
      return this->reportBytesNew(expr, ErrorTextBytesNew::fatal());
    case Bytes::None:
    case Bytes::Uint8:
    case Bytes::Int8:
    case Bytes::Uint8Clamped:
    case Bytes::Int16:
    case Bytes::Uint16:
    case Bytes::Int32:
    case Bytes::Uint32:
    case Bytes::Float16:
    case Bytes::Float32:
    case Bytes::Float64:
      return this->checkTypedArrayNew(call, type);
  }

  return this->checkTypedArrayNew(call, type);
}

Expr *BytesNewChecker::madeBuiltin(CallExpr *call, Builtin builtin, const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *BytesNewChecker::reportBytesNew(NewExpr *expr, const std::string &message) {
  this->state.getDiagnostics().error(expr->loc(), message);
  this->walkArgsLoosely(expr);

  return expr;
}

void BytesNewChecker::walkArgsLoosely(NewExpr *expr) {
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

bool BytesNewChecker::hasWrongArity(CallExpr *call, unsigned min, unsigned max) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(call->loc(), arity.error().message);
  }

  return arity.isErr();
}

void BytesNewChecker::walkNonNegative(const Expr *value, const std::string &what) {
  IndexProofs::ResultBetweenProven between =
      this->indexProofs->requireBetween(value, 0, std::nullopt, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(value->loc(), between.error().message);
  }
}

void BytesNewChecker::walkView(CallExpr *call, int64_t elementSize, const std::string &what) {
  for (size_t i = 1; i < call->args.size(); ++i) {
    call->args[i] = this->memberChecker->checkPosition(call->args[i]);
  }
  const Expr *offset = call->args.size() > 1 ? call->args[1] : nullptr;
  const Expr *length = call->args.size() > 2 ? call->args[2] : nullptr;
  this->byteProofs->walkView(call->args[0], offset, length, elementSize, what);
}

Expr *BytesNewChecker::checkArrayBufferNew(NewExpr *expr, CallExpr *call, const Type *type) {
  if (expr->args.size() == 2) {
    std::string message = ErrorTextBytesNew::resizable();
    this->state.getDiagnostics().error(expr->args[1]->loc(), message);
    this->walkArgsLoosely(expr);
    return expr;
  }
  if (this->hasWrongArity(call, 1, 1)) {
    return call;
  }
  call->args[0] = this->memberChecker->checkPosition(call->args[0]);
  std::string what = ErrorTextBytesNew::bufferLen();
  this->walkNonNegative(call->args[0], what);

  return BytesNewChecker::madeBuiltin(call, Builtin::ArrayBufferNew, type);
}

Expr *BytesNewChecker::checkDataViewNew(CallExpr *call, const Type *type) {
  if (this->hasWrongArity(call, 1, 3)) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  ResultViewedBuffer viewed = BytesNewChecker::viewedBuffer(call->args[0]->type);
  if (viewed.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), viewed.error().message);
    return call;
  }
  std::string what = ErrorTextBytesNew::dataView();
  this->walkView(call, 1, what);

  return BytesNewChecker::madeBuiltin(call, Builtin::DataViewNew, type);
}

Expr *BytesNewChecker::checkTextEncoderNew(CallExpr *call, const Type *type) {
  if (this->hasWrongArity(call, 0, 0)) {
    return call;
  }

  return BytesNewChecker::madeBuiltin(call, Builtin::TextEncoderNew, type);
}

Expr *BytesNewChecker::checkTextDecoderNew(CallExpr *call, const Type *type) {
  if (this->hasWrongArity(call, 0, 2)) {
    return call;
  }
  auto *resolved = this->state.getAst().make<IntLitExpr>(call->loc(), 1);
  resolved->type = Type::intTy();
  std::optional<Expr *> label = this->checkDecoderLabelArg(call, resolved);
  if (!label.has_value()) {
    return call;
  }
  std::vector<Expr *> checked{resolved, *label};
  bool isFatal = false;
  if (call->args.size() == 2) {
    const Type *options = BytesNewChecker::decoderOptionsType();
    Expr *given = this->exprChecker->checkExpr(call->args[1], options);
    isFatal = BytesNewChecker::mayBeFatal(given);
    std::string what = ErrorTextTextDecoder::options();
    checked.push_back(this->typeRelations->coerce(given, options, what));
  }
  call->args = checked;
  const Type *decoder = isFatal ? Type::bytesType(Bytes::FatalTextDecoder) : type;

  return BytesNewChecker::madeBuiltin(call, Builtin::TextDecoderNew, decoder);
}

const Type *BytesNewChecker::decoderOptionsType() {
  return Type::object({{"fatal", Type::optional(Type::boolTy()), false},
                       {"ignoreBOM", Type::optional(Type::boolTy()), false}});
}

std::optional<Expr *> BytesNewChecker::checkDecoderLabelArg(CallExpr *call, IntLitExpr *resolved) {
  bool isGiven = !call->args.empty() && !isa<UndefinedExpr>(call->args[0]);
  if (!isGiven) {
    if (!call->args.empty()) {
      call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    }
    return this->decoderNameLiteral(call->loc(), u"utf-8");
  }
  Expr *label = call->args[0];
  std::optional<std::u16string> text = BytesNewChecker::literalText(label);
  if (text.has_value()) {
    std::optional<std::u16string> name = this->knownDecoderName(label, *text);
    if (!name.has_value()) {
      return std::nullopt;
    }
    return this->decoderNameLiteral(call->loc(), *name);
  }
  call->args[0] = this->argumentChecker->checkArg(label, Type::string());
  resolved->value = 0;
  if (!this->decoderLabelsFit(call->args[0], label->loc())) {
    return std::nullopt;
  }

  return call->args[0];
}

std::optional<std::u16string> BytesNewChecker::knownDecoderName(const Expr *label,
                                                                const std::u16string &text) {
  std::string written = ToUtf8::toUtf8(text);
  ResultDecoderLabel decoded = BytesNewChecker::decoderLabel(written);
  if (decoded.isErr()) {
    this->state.getDiagnostics().error(label->loc(), decoded.error().message);
    return std::nullopt;
  }
  const char *found = decoded.value();

  return std::u16string(found, found + strlen(found));
}

bool BytesNewChecker::decoderLabelsFit(const Expr *checked, llvm::SMLoc at) {
  bool isSkipped = this->state.getBytesStore().isInsideDecoderParse() || checked->type->isError() ||
                   this->indexProofs->exempt(at);
  if (isSkipped) {
    return true;
  }
  std::optional<std::vector<std::string>> labels = this->literalTypes->knownTexts(checked);
  if (!labels.has_value()) {
    std::string message = ErrorTextTextDecoder::notKnown();
    this->state.getDiagnostics().error(at, message);
    return false;
  }
  for (const std::string &each : *labels) {
    ResultDecoderLabel decoded = BytesNewChecker::decoderLabel(each);
    if (decoded.isErr()) {
      this->state.getDiagnostics().error(at, decoded.error().message);
      return false;
    }
  }

  return true;
}

Expr *BytesNewChecker::decoderNameLiteral(llvm::SMLoc loc, const std::u16string &name) {
  auto *lit = this->state.getAst().make<StringLitExpr>(loc, name);
  lit->type = Type::string();

  return lit;
}

Expr *BytesNewChecker::checkTypedArrayNew(CallExpr *call, const Type *type) {
  if (call->args.empty()) {
    auto *zero = this->state.getAst().make<IntLitExpr>(call->loc(), 0);
    zero->type = Type::intTy();
    call->args = {zero};
    return BytesNewChecker::madeBuiltin(call, Builtin::BytesNew, type);
  }
  if (call->args.size() > 3) {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(call->loc(), arity.error().message);
    }
    return call;
  }
  auto *values = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (values != nullptr && call->args.size() == 1) {
    return this->checkBytesValues(call, values, type);
  }
  Expr *&source = call->args[0];
  this->iteratorMethods->markFused(source);
  source = this->exprChecker->checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    std::string what = ErrorTextBytesNew::the(type->name());
    this->walkView(call, type->elementSize(), what);
    return BytesNewChecker::madeBuiltin(call, Builtin::BytesView, type);
  }
  if (call->args.size() > 1) {
    std::string message = ErrorTextBytesNew::offsetOnly(type->name(), from->name());
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return call;
  }
  if (from->isInteger()) {
    return this->checkLengthNew(call, type);
  }
  if (from->isFloat()) {
    std::string message = ErrorTextBytesNew::floatLen();
    this->state.getDiagnostics().error(source->loc(), message);
    return call;
  }
  if (!this->checkBytesSource(source, type)) {
    return call;
  }
  Builtin builtin = from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return BytesNewChecker::madeBuiltin(call, builtin, type);
}

Expr *BytesNewChecker::checkLengthNew(CallExpr *call, const Type *type) {
  call->args[0] = this->typeRelations->convert(call->args[0], Type::intTy());
  std::string what = ErrorTextBytesNew::lengthOf(type->name());
  this->walkNonNegative(call->args[0], what);
  auto *zeroed = this->state.getAst().make<BoolLitExpr>(call->loc(), true);
  zeroed->type = Type::boolTy();
  call->args.push_back(zeroed);

  return BytesNewChecker::madeBuiltin(call, Builtin::BytesNew, type);
}

Expr *BytesNewChecker::checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type) {
  const Type *element =
      type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
  std::string what = ErrorTextByteView::element(type->name());
  call->type = Type::error();
  bool isSpreading =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (!isSpreading) {
    std::vector<Expr *> values;
    for (ArrayLitExpr::Element &e : lit->elements) {
      if (e.hole) {
        std::string message = ErrorTextArrayHole::hole();
        this->state.getDiagnostics().error(e.expr->loc(), message);
        return call;
      }
      Expr *value = this->argumentChecker->checkArg(e.expr, element);
      ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(value, type, what);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(value->loc(), fitting.error().message);
      }
      values.push_back(value);
    }
    call->args = std::move(values);
    call->target = BuiltinCall{.builtin = Builtin::BytesOf};
    call->type = type;
    return call;
  }
  Expr *array = this->exprChecker->checkExpr(lit, Type::array(element));
  if (array->type->isError()) {
    return call;
  }
  bool isOk = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(e.expr, type, what);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(e.expr->loc(), fitting.error().message);
      }
    } else {
      isOk &= this->checkBytesSource(e.expr, type);
    }
  }
  if (!isOk) {
    return call;
  }
  call->args = {array};
  call->target = BuiltinCall{.builtin = Builtin::BytesFromArray};
  call->type = type;

  return call;
}

bool BytesNewChecker::checkBytesSource(Expr *source, const Type *target) {
  const Type *from = source->type;
  if (from->isError()) {
    return false;
  }
  const Type *element = from->isTypedArray() ? from->elementType()
                        : from->isArray()    ? from->inner()
                        : from->isIterator() ? from->yielded()
                                             : nullptr;
  if (!element) {
    std::string message =
        ErrorTextBytesSource::notSource(target->name(), from->name(), from->isString());
    this->state.getDiagnostics().error(source->loc(), message);
    return false;
  }
  if (from->isIterator()) {
    std::string what = ErrorTextBytesSource::quoted(target->name());
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->generatorChecker->checkSendsNothing(from, what);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(source->loc(), nothing.error().message);
    }
  }
  if (!element->isNumeric()) {
    std::string message = ErrorTextBytesSource::notNumbers(from->name(), target->name());
    this->state.getDiagnostics().error(source->loc(), message);
    return false;
  }
  int64_t lo, hi;
  if (!target->elementRange(lo, hi)) {
    return true;
  }
  Bytes kind = target->bytes();
  std::string text = BytesCheckHelpers::valueText(source);
  std::string wrap = BytesCheckHelpers::wrapHint(kind, "x");
  std::string hint = ErrorTextBytesSource::hint(target->name(), text, wrap);
  if (element->isFloat()) {
    std::string message = ErrorTextBytesSource::doubles(from->name(), target->name(), text);
    this->state.getDiagnostics().error(source->loc(), message);
    return false;
  }
  if (kind == Bytes::Uint8Clamped) {
    return true;
  }
  int64_t slo, shi;
  if (from->isTypedArray() && from->elementRange(slo, shi)) {
    if (slo >= lo && shi <= hi) {
      return true;
    }
    std::string fromRange = ErrorTextBytes::range(slo, shi);
    std::string range = ErrorTextBytes::range(lo, hi);
    std::string message =
        ErrorTextBytesSource::mayNotFit(from->name(), fromRange, target->name(), range, hint);
    this->state.getDiagnostics().error(source->loc(), message);
    return false;
  }
  std::string range = ErrorTextBytes::range(lo, hi);
  std::string message = ErrorTextBytesSource::unproven(from->name(), target->name(), range, hint);
  this->state.getDiagnostics().error(source->loc(), message);

  return false;
}

}
