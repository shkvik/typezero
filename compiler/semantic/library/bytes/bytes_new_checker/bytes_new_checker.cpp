#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "compiler/semantic/declare/prelude/prelude.h"
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
  this->prelude = &all.getPrelude();
  this->byteProofs = &all.getByteProofs();
  this->exprChecker = &all.getExprChecker();
  this->indexProofs = &all.getIndexProofs();
  this->literalTypes = &all.getLiteralTypes();
  this->memberChecker = &all.getMemberChecker();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->iteratorMethods = &all.getIteratorMethods();
  this->generatorChecker = &all.getGeneratorChecker();
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
  if (options->type->isError()) {
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  if (!literal) {
    const Field *field = options->type->field("fatal");
    return field && !field->type->isUndefined();
  }
  bool fatal = false;
  for (const ObjectLitExpr::Property &prop : literal->props) {
    if (prop.spread()) {
      fatal = fatal || BytesNewChecker::mayBeFatal(prop.value);
      continue;
    }
    if (prop.name != "fatal") {
      continue;
    }
    const Expr *value = prop.value;
    while (auto *conv = dyn_cast<CastExpr>(value)) {
      if (!conv->isImplicit()) {
        break;
      }
      value = conv->operand;
    }
    auto *flag = dyn_cast<BoolLitExpr>(value);
    fatal = !isa<UndefinedExpr>(value) && !(flag && !flag->value);
  }

  return fatal;
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
  std::vector<Expr *> &args = expr->args;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  auto *callee = this->state.getAst().make<NameExpr>(loc, type->name());
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, args);
  if (!expr->typeArgs.empty()) {
    std::string message = ErrorTextBytesNew::typeArgs(type->name());
    this->state.getDiagnostics().error(loc, message);
  }
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  call->type = Type::error();
  switch (type->bytes()) {
    case Bytes::Buffer: {
      std::string message = ErrorTextBytesNew::buffer();
      this->state.getDiagnostics().error(loc, message);
      checkArgsLoosely();
      return expr;
    }
  case Bytes::ArrayBuffer: {
    if (args.size() == 2) {
      std::string message = ErrorTextBytesNew::resizable();
      this->state.getDiagnostics().error(args[1]->loc(), message);
      checkArgsLoosely();
      return expr;
    }
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    call->args[0] = this->memberChecker->checkPosition(call->args[0]);
    std::string what = ErrorTextBytesNew::bufferLen();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    return made(Builtin::ArrayBufferNew, type);
  }
  case Bytes::DataView: {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    ResultViewedBuffer viewed = BytesNewChecker::viewedBuffer(call->args[0]->type);
    if (viewed.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), viewed.error().message);
      return call;
    }
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = this->memberChecker->checkPosition(call->args[i]);
    }
    std::string what = ErrorTextBytesNew::dataView();
    this->byteProofs->checkView(call->args[0], call->args.size() > 1 ? call->args[1] : nullptr,
                                call->args.size() > 2 ? call->args[2] : nullptr, 1, what);
    return made(Builtin::DataViewNew, type);
  }
  case Bytes::TextEncoder: {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return made(Builtin::TextEncoderNew, type);
  }
  case Bytes::TextDecoder: {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    auto *resolved = this->state.getAst().make<IntLitExpr>(loc, 1);
    resolved->type = Type::intTy();
    std::u16string name = u"utf-8";
    std::vector<Expr *> checked{resolved, nullptr};
    if (!call->args.empty() && !isa<UndefinedExpr>(call->args[0])) {
      Expr *label = call->args[0];
      if (std::optional<std::u16string> text = BytesNewChecker::literalText(label)) {
        std::string written = ToUtf8::toUtf8(*text);
        ResultDecoderLabel decoded = BytesNewChecker::decoderLabel(written);
        if (decoded.isErr()) {
          this->state.getDiagnostics().error(label->loc(), decoded.error().message);
          return call;
        }
        const char *found = decoded.value();
        name = std::u16string(found, found + strlen(found));
      } else {
        call->args[0] = this->argumentChecker->checkArg(label, Type::string());
        resolved->value = 0;
        checked[1] = call->args[0];
        if (!this->state.getBytesStore().isInsideDecoderParse() &&
            !call->args[0]->type->isError() && !this->indexProofs->exempt(label->loc())) {
          std::optional<std::vector<std::string>> labels =
              this->literalTypes->knownTexts(call->args[0]);
          if (!labels) {
            std::string message = ErrorTextTextDecoder::notKnown();
            this->state.getDiagnostics().error(label->loc(), message);
            return call;
          }
          for (const std::string &each : *labels) {
            ResultDecoderLabel decoded = BytesNewChecker::decoderLabel(each);
            if (decoded.isErr()) {
              this->state.getDiagnostics().error(label->loc(), decoded.error().message);
              return call;
            }
          }
        }
      }
    } else if (!call->args.empty()) {
      call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    }
    if (!checked[1]) {
      auto *lit = this->state.getAst().make<StringLitExpr>(loc, name);
      lit->type = Type::string();
      checked[1] = lit;
    }
    bool fatal = false;
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"fatal", Type::optional(Type::boolTy()), false},
                                          {"ignoreBOM", Type::optional(Type::boolTy()), false}});
      Expr *given = this->exprChecker->checkExpr(call->args[1], options);
      fatal = BytesNewChecker::mayBeFatal(given);
      std::string what = ErrorTextTextDecoder::options();
      checked.push_back(this->typeRelations->coerce(given, options, what));
    }
    call->args = checked;
    return made(Builtin::TextDecoderNew, fatal ? Type::bytesType(Bytes::FatalTextDecoder) : type);
  }
  case Bytes::FatalTextDecoder: {
    std::string message = ErrorTextBytesNew::fatal();
    this->state.getDiagnostics().error(loc, message);
    checkArgsLoosely();
    return expr;
  }
  default:
    break;
  }

  if (args.empty()) {
    auto *zero = this->state.getAst().make<IntLitExpr>(loc, 0);
    zero->type = Type::intTy();
    call->args = {zero};
    return made(Builtin::BytesNew, type);
  }
  if (args.size() > 3) {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    return call;
  }
  Expr *&source = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && args.size() == 1) {
    return this->checkBytesValues(call, lit, type);
  }
  this->iteratorMethods->markFused(source);
  source = this->exprChecker->checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = this->memberChecker->checkPosition(call->args[i]);
    }
    std::string what = ErrorTextBytesNew::the(type->name());
    this->byteProofs->checkView(source, call->args.size() > 1 ? call->args[1] : nullptr,
                                call->args.size() > 2 ? call->args[2] : nullptr,
                                type->elementSize(), what);
    return made(Builtin::BytesView, type);
  }
  if (call->args.size() > 1) {
    std::string message = ErrorTextBytesNew::offsetOnly(type->name(), from->name());
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return call;
  }
  if (from->isInteger()) {
    source = this->typeRelations->convert(source, Type::intTy());
    std::string what = ErrorTextBytesNew::lengthOf(type->name());
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(source, 0, std::nullopt, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(source->loc(), between.error().message);
    }
    auto *zeroed = this->state.getAst().make<BoolLitExpr>(loc, true);
    zeroed->type = Type::boolTy();
    call->args.push_back(zeroed);
    return made(Builtin::BytesNew, type);
  }
  if (from->isFloat()) {
    std::string message = ErrorTextBytesNew::floatLen();
    this->state.getDiagnostics().error(source->loc(), message);
    return call;
  }
  if (!this->checkBytesSource(source, type)) {
    return call;
  }

  return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, type);
}

Expr *BytesNewChecker::checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type) {
  const Type *element =
      type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
  std::string what = ErrorTextByteView::element(type->name());
  call->type = Type::error();
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (!spreads) {
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
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      ByteProofs::ResultValueFitsType fitting = this->byteProofs->valueFits(e.expr, type, what);
      if (fitting.isErr()) {
        this->state.getDiagnostics().error(e.expr->loc(), fitting.error().message);
      }
    } else {
      ok &= this->checkBytesSource(e.expr, type);
    }
  }
  if (!ok) {
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
