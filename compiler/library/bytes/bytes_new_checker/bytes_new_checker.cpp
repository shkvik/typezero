#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
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

BytesNewChecker::BytesNewChecker(Checker &checker) : checker_(checker) {}

Expr *BytesNewChecker::checkBytesNew(NewExpr *expr, const Type *type) {
  llvm::SMLoc loc = expr->loc();
  std::vector<Expr *> &args = expr->args;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  auto *callee = checker_.ctx_.make<NameExpr>(loc, type->name());
  // A fake call, for the helpers that check arguments.
  auto *call = checker_.ctx_.make<CallExpr>(loc, callee, args);
  if (!expr->typeArgs.empty()) {
    checker_.diags_.error(loc, "'new " + type->name() + "' takes no type arguments");
  }
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;

    return call;
  };
  call->type = Type::error();
  switch (type->bytes()) {
  case Bytes::Buffer:
    checker_.diags_.error(loc,
                          "'new Buffer(...)' is not supported, as in node, where it is deprecated: "
                          "make one with 'Buffer.from(values)' or 'Buffer.alloc(size)'");
    checkArgsLoosely();
    return expr;
  case Bytes::ArrayBuffer:
    if (args.size() == 2) {
      checker_.diags_.error(args[1]->loc(),
                            "an ArrayBuffer that can change its size is not supported yet "
                            "(docs/bytes.md, section 7); make a new one of the size wanted");
      checkArgsLoosely();
      return expr;
    }
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    call->args[0] = checker_.memberChecker().checkPosition(call->args[0]);
    if (std::optional<Linear> n = checker_.indexProofs().linear(call->args[0]);
        n && n->term.isZero() && n->offset < 0) {
      checker_.diags_.error(call->args[0]->loc(),
                            "the length of an ArrayBuffer cannot be negative");
    }
    return made(Builtin::ArrayBufferNew, type);
  case Bytes::DataView: {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 3)) {
      return call;
    }
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    const Type *buffer = call->args[0]->type;
    if (buffer->bytes() != Bytes::ArrayBuffer && !buffer->isError()) {
      checker_.diags_.error(
          call->args[0]->loc(),
          "a DataView is made over an ArrayBuffer, found '" + buffer->name() + "'" +
              (buffer->isTypedArray() ? "; give its buffer: 'new DataView(u.buffer, "
                                        "u.byteOffset, u.byteLength)'"
                                      : ""));
      return call;
    }
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = checker_.memberChecker().checkPosition(call->args[i]);
    }
    return made(Builtin::DataViewNew, type);
  }
  case Bytes::TextEncoder:
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 0)) {
      return call;
    }
    return made(Builtin::TextEncoderNew, type);
  case Bytes::TextDecoder: {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 2)) {
      return call;
    }
    // The label: one written in the code is looked up now, as its table is
    // built into the program only then (docs/bytes.md, section 5).
    auto *resolved = checker_.ctx_.make<IntLitExpr>(loc, 1);
    resolved->type = Type::intTy();
    std::u16string name = u"utf-8";
    std::vector<Expr *> checked{resolved, nullptr};
    if (!call->args.empty() && !isa<UndefinedExpr>(call->args[0])) {
      Expr *label = call->args[0];
      if (std::optional<std::u16string> text = literalText(label)) {
        std::string lower = llvm::StringRef(utf8(*text)).trim(" \t\n\f\r").lower();
        const char *found = tz_encoding_name(lower.c_str());
        if (!found) {
          checker_.diags_.error(label->loc(),
                                "'" + utf8(*text) +
                                    "' is not an encoding TextDecoder knows (JavaScript "
                                    "throws a RangeError); e.g. 'utf-8', 'utf-16le', "
                                    "'windows-1251', 'latin1'");
          return call;
        }
        name = std::u16string(found, found + strlen(found));
      } else {
        // Known only when the program runs: the program then holds the
        // tables of all the encodings.
        call->args[0] = checker_.argumentChecker().checkArg(label, Type::string());
        resolved->value = 0;
        checked[1] = call->args[0];
      }
    } else if (!call->args.empty()) {
      call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    }
    if (!checked[1]) {
      auto *lit = checker_.ctx_.make<StringLitExpr>(loc, name);
      lit->type = Type::string();
      checked[1] = lit;
    }
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"fatal", Type::optional(Type::boolTy()), false},
                                          {"ignoreBOM", Type::optional(Type::boolTy()), false}});
      Expr *given = checker_.exprChecker().checkExpr(call->args[1], options);
      checked.push_back(
          checker_.typeRelations().coerce(given, options, "the options of TextDecoder"));
    }
    call->args = checked;
    return made(Builtin::TextDecoderNew, type);
  }
  default:
    break;
  }

  // A typed array: of a length, of values, of another typed array, or a view
  // of an ArrayBuffer.
  if (args.empty()) {
    auto *zero = checker_.ctx_.make<IntLitExpr>(loc, 0);
    zero->type = Type::longTy();
    call->args = {zero};
    return made(Builtin::BytesNew, type);
  }
  if (args.size() > 3) {
    checker_.argumentChecker().checkArity(call, loc, 0, 3);
    return call;
  }
  Expr *&source = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && args.size() == 1) {
    return checkBytesValues(call, lit, type);
  }
  checker_.iteratorMethods().markFused(source);
  source = checker_.exprChecker().checkExpr(source);
  const Type *from = source->type;
  if (from->isError()) {
    return call;
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = checker_.memberChecker().checkPosition(call->args[i]);
    }
    return made(Builtin::BytesView, type);
  }
  if (call->args.size() > 1) {
    checker_.diags_.error(call->args[1]->loc(), "'new " + type->name() +
                                                    "' takes an offset and a length only with an "
                                                    "ArrayBuffer, found '" +
                                                    from->name() + "'");
    return call;
  }
  if (from->isInteger()) {
    source = checker_.typeRelations().convert(source, Type::longTy());
    if (std::optional<Linear> n = checker_.indexProofs().linear(source);
        n && n->term.isZero() && n->offset < 0) {
      checker_.diags_.error(source->loc(), "the length of a typed array cannot be negative");
    }
    auto *zeroed = checker_.ctx_.make<BoolLitExpr>(loc, true);
    zeroed->type = Type::boolTy();
    call->args.push_back(zeroed);
    return made(Builtin::BytesNew, type);
  }
  if (from->isFloat()) {
    checker_.diags_.error(source->loc(),
                          "the length of a typed array must be an integer, found 'double'; "
                          "round it with Math.floor(x) or drop the fraction with 'x | 0'");
    return call;
  }
  if (!checkBytesSource(source, type)) {
    return call;
  }

  return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, type);
}

Expr *BytesNewChecker::checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type) {
  const Type *element = type->elementType();
  std::string what = "an element of '" + type->name().str() + "'";
  call->type = Type::error();
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (!spreads) {
    std::vector<Expr *> values;
    for (ArrayLitExpr::Element &e : lit->elements) {
      if (e.hole) {
        checker_.diags_.error(e.expr->loc(),
                              "arrays cannot have holes; write the element explicitly");
        return call;
      }
      Expr *value = checker_.argumentChecker().checkArg(e.expr, element);
      checker_.byteProofs().checkFits(value, type, what);
      values.push_back(value);
    }
    call->args = std::move(values);
    call->builtin = Builtin::BytesOf;
    call->type = type;
    return call;
  }
  // With spreads, an array of the values first.
  Expr *array = checker_.exprChecker().checkExpr(lit, Type::array(element));
  if (array->type->isError()) {
    return call;
  }
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      checker_.byteProofs().checkFits(e.expr, type, what);
    } else {
      ok &= checkBytesSource(e.expr, type);
    }
  }
  if (!ok) {
    return call;
  }
  call->args = {array};
  call->builtin = Builtin::BytesFromArray;
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
    checker_.diags_.error(source->loc(),
                          "a " + target->name() +
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
    checker_.generatorChecker().checkSendsNothing(from, source->loc(),
                                                  "'" + target->name().str() + "'");
  }
  if (!element->isNumeric()) {
    checker_.diags_.error(source->loc(), "the elements of '" + from->name() +
                                             "' are not numbers, which a " + target->name() +
                                             " holds");
    return false;
  }
  int64_t lo, hi;
  if (!target->elementRange(lo, hi)) {
    return true; // a floating element takes any number
  }
  Bytes kind = target->bytes();
  std::string text = valueText(source);
  std::string hint = target->name().str() + ".from(" + text + ", x => " + wrapHint(kind, "x") + ")";
  if (element->isFloat()) {
    checker_.diags_.error(source->loc(),
                          "the elements of '" + from->name() +
                              "' are doubles, which an "
                              "element of '" +
                              target->name() +
                              "' cannot hold without an explicit conversion; convert them, "
                              "e.g. '" +
                              target->name() + ".from(" + text + ", x => Math.round(x) & 0xff)'");
    return false;
  }
  // An Int32Array takes any int, and a clamped element any integer.
  if (kind == Bytes::Uint8Clamped || (kind == Bytes::Int32 && element->isInt())) {
    return true;
  }
  int64_t slo, shi;
  if (from->isTypedArray() && from->elementRange(slo, shi)) {
    if (slo >= lo && shi <= hi) {
      return true;
    }
    checker_.diags_.error(source->loc(),
                          "the elements of '" + from->name() + "' (" + rangeText(slo, shi) +
                              ") may not fit in an element of '" + target->name() + "' (" +
                              rangeText(lo, hi) +
                              "), and JavaScript would cut them without a word; convert "
                              "them explicitly, e.g. '" +
                              hint + "'");
    return false;
  }
  checker_.diags_.error(source->loc(),
                        "cannot prove that the elements of '" + from->name() + "' fit in " +
                            "an element of '" + target->name() + "' (" + rangeText(lo, hi) +
                            "), and JavaScript would cut them without a word; write the "
                            "values in the literal, or convert them explicitly, e.g. '" +
                            hint + "'");

  return false;
}

} // namespace tz
