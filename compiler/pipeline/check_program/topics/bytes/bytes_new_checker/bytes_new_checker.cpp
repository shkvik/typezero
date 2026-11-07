#include "compiler/pipeline/check_program/topics/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterator_methods/iterator_methods.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string BytesNewChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
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

std::string BytesNewChecker::rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
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

BytesNewChecker::BytesNewChecker(CheckState &owner)
    : state(owner) {}

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
  result->type = Type::result(result->type, Prelude(this->state).builtinType("EncodingError"));

  return result;
}

Expr *BytesNewChecker::checkBytesNew(NewExpr *expr, const Type *type) {
  llvm::SMLoc loc = expr->loc();
  std::vector<Expr *> &args = expr->args;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  auto *callee = this->state.getAst().make<NameExpr>(loc, type->name());
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, args);
  if (!expr->typeArgs.empty()) {
    this->state.getDiagnostics().error(loc, "'new " + type->name() + "' takes no type arguments");
  }
  auto made = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  call->type = Type::error();
  switch (type->bytes()) {
  case Bytes::Buffer:
    this->state.getDiagnostics().error(
        loc, "'new Buffer(...)' is not supported, as in node, where it is deprecated: "
             "make one with 'Buffer.from(values)' or 'Buffer.alloc(size)'");
    checkArgsLoosely();
    return expr;
  case Bytes::ArrayBuffer: {
    if (args.size() == 2) {
      this->state.getDiagnostics().error(
          args[1]->loc(), "an ArrayBuffer that can change its size is not supported yet "
                          "(docs/bytes.md, section 7); make a new one of the size wanted");
      checkArgsLoosely();
      return expr;
    }
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state)
            .requireBetween(call->args[0], 0, std::nullopt, "the length of an ArrayBuffer");
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    return made(Builtin::ArrayBufferNew, type);
  }
  case Bytes::DataView: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    const Type *buffer = call->args[0]->type;
    if (buffer->bytes() != Bytes::ArrayBuffer && !buffer->isError()) {
      this->state.getDiagnostics().error(
          call->args[0]->loc(),
          "a DataView is made over an ArrayBuffer, found '" + buffer->name() + "'" +
              (buffer->isTypedArray() ? "; give its buffer: 'new DataView(u.buffer, "
                                        "u.byteOffset, u.byteLength)'"
                                      : ""));
      return call;
    }
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
    }
    ByteProofs(this->state)
        .checkView(call->args[0], call->args.size() > 1 ? call->args[1] : nullptr,
                   call->args.size() > 2 ? call->args[2] : nullptr, 1, "the DataView");
    return made(Builtin::DataViewNew, type);
  }
  case Bytes::TextEncoder: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return made(Builtin::TextEncoderNew, type);
  }
  case Bytes::TextDecoder: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 2);
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
        std::string lower = llvm::StringRef(BytesNewChecker::utf8(*text)).trim(" \t\n\f\r").lower();
        const char *found = tz_encoding_name(lower.c_str());
        if (!found) {
          this->state.getDiagnostics().error(
              label->loc(), "'" + BytesNewChecker::utf8(*text) +
                                "' is not an encoding TextDecoder knows (JavaScript "
                                "throws a RangeError); e.g. 'utf-8', 'utf-16le', "
                                "'windows-1251', 'latin1'");
          return call;
        }
        name = std::u16string(found, found + strlen(found));
      } else {
        call->args[0] = ArgumentChecker(this->state).checkArg(label, Type::string());
        resolved->value = 0;
        checked[1] = call->args[0];
        if (!this->state.getBytesStore().isInsideDecoderParse() &&
            !call->args[0]->type->isError() && !IndexProofs(this->state).exempt(label->loc())) {
          std::optional<std::vector<std::string>> labels =
              LiteralTypes(this->state).knownTexts(call->args[0]);
          if (!labels) {
            this->state.getDiagnostics().error(
                label->loc(), "a label known only when the program runs may name no "
                              "encoding, which JavaScript would throw a RangeError for; "
                              "parse it with 'TextDecoder.parse(label)', which gives a "
                              "Result, or check it first, e.g. 'if (label == \"utf-8\" || "
                              "label == \"windows-1251\")'");
            return call;
          }
          for (const std::string &each : *labels) {
            std::string lower = llvm::StringRef(each).trim(" \t\n\f\r").lower();
            if (!tz_encoding_name(lower.c_str())) {
              this->state.getDiagnostics().error(
                  label->loc(), "'" + each +
                                    "' is not an encoding TextDecoder knows (JavaScript "
                                    "throws a RangeError); e.g. 'utf-8', 'utf-16le', "
                                    "'windows-1251', 'latin1'");
              return call;
            }
          }
        }
      }
    } else if (!call->args.empty()) {
      call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
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
      Expr *given = ExprChecker(this->state).checkExpr(call->args[1], options);
      fatal = BytesNewChecker::mayBeFatal(given);
      checked.push_back(
          TypeRelations(this->state).coerce(given, options, "the options of TextDecoder"));
    }
    call->args = checked;
    return made(Builtin::TextDecoderNew, fatal ? Type::bytesType(Bytes::FatalTextDecoder) : type);
  }
  case Bytes::FatalTextDecoder:
    this->state.getDiagnostics().error(
        loc, "a FatalTextDecoder is made by 'new TextDecoder(label, { fatal: "
             "true })'");
    checkArgsLoosely();
    return expr;
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
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    return call;
  }
  Expr *&source = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && args.size() == 1) {
    return this->checkBytesValues(call, lit, type);
  }
  IteratorMethods(this->state).markFused(source);
  source = ExprChecker(this->state).checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = MemberChecker(this->state).checkPosition(call->args[i]);
    }
    ByteProofs(this->state)
        .checkView(source, call->args.size() > 1 ? call->args[1] : nullptr,
                   call->args.size() > 2 ? call->args[2] : nullptr, type->elementSize(),
                   "the " + type->name().str());
    return made(Builtin::BytesView, type);
  }
  if (call->args.size() > 1) {
    this->state.getDiagnostics().error(call->args[1]->loc(),
                                       "'new " + type->name() +
                                           "' takes an offset and a length only with an "
                                           "ArrayBuffer, found '" +
                                           from->name() + "'");
    return call;
  }
  if (from->isInteger()) {
    source = TypeRelations(this->state).convert(source, Type::intTy());
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state)
            .requireBetween(source, 0, std::nullopt, "the length of a " + type->name().str());
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(source->loc(), between.error().message);
    }
    auto *zeroed = this->state.getAst().make<BoolLitExpr>(loc, true);
    zeroed->type = Type::boolTy();
    call->args.push_back(zeroed);
    return made(Builtin::BytesNew, type);
  }
  if (from->isFloat()) {
    this->state.getDiagnostics().error(
        source->loc(), "the length of a typed array must be an integer, found 'double'; "
                       "round it with Math.floor(x) or drop the fraction with 'x | 0'");
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
  std::string what = "an element of '" + type->name().str() + "'";
  call->type = Type::error();
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (!spreads) {
    std::vector<Expr *> values;
    for (ArrayLitExpr::Element &e : lit->elements) {
      if (e.hole) {
        this->state.getDiagnostics().error(
            e.expr->loc(), "arrays cannot have holes; write the element explicitly");
        return call;
      }
      Expr *value = ArgumentChecker(this->state).checkArg(e.expr, element);
      ByteProofs(this->state).checkFits(value, type, what);
      values.push_back(value);
    }
    call->args = std::move(values);
    call->target = BuiltinCall{.builtin = Builtin::BytesOf};
    call->type = type;
    return call;
  }
  Expr *array = ExprChecker(this->state).checkExpr(lit, Type::array(element));
  if (array->type->isError()) {
    return call;
  }
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      ByteProofs(this->state).checkFits(e.expr, type, what);
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
    this->state.getDiagnostics().error(
        source->loc(), "a " + target->name() +
                           " is made from a length, an array or an iterator of numbers, "
                           "another typed array or an ArrayBuffer, found '" +
                           from->name() + "'" +
                           (from->isString() ? "; for the bytes of a string, use 'new "
                                               "TextEncoder().encode(s)' or "
                                               "'Buffer.from(s)'"
                                             : ""));
    return false;
  }
  if (from->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        GeneratorChecker(this->state).checkSendsNothing(from, "'" + target->name().str() + "'");
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(source->loc(), nothing.error().message);
    }
  }
  if (!element->isNumeric()) {
    this->state.getDiagnostics().error(source->loc(), "the elements of '" + from->name() +
                                                          "' are not numbers, which a " +
                                                          target->name() + " holds");
    return false;
  }
  int64_t lo, hi;
  if (!target->elementRange(lo, hi)) {
    return true;
  }
  Bytes kind = target->bytes();
  std::string text = valueText(source);
  std::string hint = target->name().str() + ".from(" + text + ", x => " + wrapHint(kind, "x") + ")";
  if (element->isFloat()) {
    this->state.getDiagnostics().error(
        source->loc(), "the elements of '" + from->name() +
                           "' are doubles, which an "
                           "element of '" +
                           target->name() +
                           "' cannot hold without an explicit conversion; convert them, "
                           "e.g. '" +
                           target->name() + ".from(" + text + ", x => Math.round(x) & 0xff)'");
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
    this->state.getDiagnostics().error(
        source->loc(), "the elements of '" + from->name() + "' (" +
                           BytesNewChecker::rangeText(slo, shi) +
                           ") may not fit in an element of '" + target->name() + "' (" +
                           BytesNewChecker::rangeText(lo, hi) +
                           "), and JavaScript would cut them without a word; convert "
                           "them explicitly, e.g. '" +
                           hint + "'");
    return false;
  }
  this->state.getDiagnostics().error(
      source->loc(), "cannot prove that the elements of '" + from->name() + "' fit in " +
                         "an element of '" + target->name() + "' (" +
                         BytesNewChecker::rangeText(lo, hi) +
                         "), and JavaScript would cut them without a word; write the "
                         "values in the literal, or convert them explicitly, e.g. '" +
                         hint + "'");

  return false;
}

}
