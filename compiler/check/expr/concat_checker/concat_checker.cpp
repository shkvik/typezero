#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// The source text of the two sides of `bin`, an operator `length` characters
// long; empty if the source is not known.
std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source, llvm::SMLoc op,
                                                         size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

// An operand as it was before toStringOperand converted it.
const Expr *unconverted(const Expr *expr) {
  auto *cast = dyn_cast<CastExpr>(expr);

  return cast && cast->isImplicit() && cast->type->isString() ? cast->operand : expr;
}

// The parts `+` joins in the chain ending at `bin`, with their source text:
// for `a + b + "x" + c` they are `a + b` (a sum: the numbers before the first
// string add up, as in JavaScript), "x" and c.
void concatParts(const BinaryExpr *bin,
                 std::vector<std::pair<const Expr *, llvm::StringRef>> &parts) {
  auto [lhs, rhs] = operandTexts(bin->source, bin->loc(), 1);
  auto *left = dyn_cast<BinaryExpr>(unconverted(bin->lhs));
  if (left && left->op == BinaryOp::Add && left->type && left->type->isString()) {
    concatParts(left, parts);
  } else {
    parts.push_back({unconverted(bin->lhs), lhs});
  }
  parts.push_back({unconverted(bin->rhs), rhs});
}

// The parts as one template literal, or "" if some text is not known. If
// the template would hold just one value, `only` gets its text.
std::string concatTemplate(llvm::ArrayRef<std::pair<const Expr *, llvm::StringRef>> parts,
                           llvm::StringRef &only) {
  std::string result = "`";
  unsigned values = 0;
  for (auto [expr, text] : parts) {
    if (text.empty()) {
      return "";
    }
    bool quoted = text.size() >= 2 && text.back() == text.front();
    if (isa<StringLitExpr>(expr) && quoted && (text.front() == '"' || text.front() == '\'')) {
      // The escapes of the literal mean the same in a template; a backquote
      // and `${` would not.
      llvm::StringRef body = text.drop_front().drop_back();
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
          result += body.substr(i, 2);
          ++i;
          continue;
        }
        if (body[i] == '`' || (body[i] == '$' && body.substr(i + 1).starts_with("{"))) {
          result += '\\';
        }
        result += body[i];
      }
    } else if (isa<TemplateExpr>(expr) && quoted && text.front() == '`') {
      result += text.drop_front().drop_back();
    } else {
      only = withoutParens(text);
      ++values;
      result += ("${" + only + "}").str();
    }
  }
  if (values != 1 || result.size() != only.size() + 4) { // "`${" and "}"
    only = "";
  }

  return result + "`";
}

} // namespace

ConcatChecker::ConcatChecker(Checker &checker) : checker_(checker) {}

Expr *ConcatChecker::toStringOperand(Expr *expr) {
  const Type *type = expr->type;
  if (type->isString() || type->isError()) {
    return expr;
  }
  if (type->isVoid()) {
    checker_.diags_.error(expr->loc(),
                          "an expression of type 'void' cannot be converted to a string");
    return expr;
  }
  if (type->isRegExp()) {
    return checker_.regexChecker().regexToString(expr);
  }
  // As in JavaScript: the text of toString(), in local time.
  if (type->isDate()) {
    return checker_.dateChecker().dateToString(expr);
  }
  if (type->isBytes()) {
    return checker_.bytesChecker().bytesToString(expr);
  }
  // As TypeScript allows; an instance for objects or functions reports it at
  // the call.
  if (type->nonOptional()->isParam()) {
    checker_.genericChecker().noteForced(expr, Type::string());
    return checker_.typeRelations().convert(expr, Type::string());
  }
  if (Checker::holdsFunctions(type)) {
    // JavaScript would give the source code of the function.
    checker_.diags_.error(expr->loc(), "a function cannot be converted to a string; print it with "
                                       "console.log, or use its name");
    return expr;
  }
  if (type->nonOptional()->isTuple()) {
    checker_.diags_.error(
        expr->loc(), "a tuple cannot be converted to a string; print it with console.log, or build "
                     "the text from its elements");
    return expr;
  }
  if (Checker::holdsResults(type)) {
    checker_.diags_.error(expr->loc(),
                          "a Result cannot be converted to a string: JavaScript would give "
                          "'[object Object]'; print it with console.log, or make the text with "
                          "'match'");
    return expr;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type->nonOptional());
      cls && !ClassDeclarer::methodsNamed(cls, "toString", /*isStatic=*/false).empty()) {
    // JavaScript would call it: tz calls no code by itself.
    auto *name = dyn_cast<NameExpr>(expr);
    std::string text = name ? name->name.str() : "o";
    checker_.diags_.error(expr->loc(),
                          "an object is not converted to a string by calling its toString(): "
                          "tz calls no code by itself; call it, e.g. '${" +
                              text + ".toString()}'");
    return expr;
  }
  if (const Type *dict = type->nonOptional(); dict->isDict()) {
    const char *kind = dict->isRecord() ? "Record" : dict->isMap() ? "Map" : "Set";
    checker_.diags_.error(expr->loc(),
                          std::string("a ") + kind +
                              " cannot be converted to a string: JavaScript would give "
                              "'[object " +
                              (dict->isRecord() ? "Object" : kind) +
                              "]'; print it with console.log, or build the text from its "
                              "entries");
    return expr;
  }
  if (type->nonOptional()->isIterator()) {
    checker_.diags_.error(expr->loc(),
                          "an iterator cannot be converted to a string: JavaScript would give "
                          "'[object Generator]'; collect its values first, e.g. "
                          "'[...it].join()'");
    return expr;
  }
  if (UnknownChecker::holdsUnknown(type)) {
    checker_.diags_.error(expr->loc(),
                          "an unknown value cannot be converted to a string: it may hold an "
                          "object or a function; check what it holds first, e.g. 'typeof x == "
                          "\"string\"'");
    return expr;
  }
  if (JsonChecker::holdsJson(type)) {
    // An object would be "[object Object]" in JavaScript.
    checker_.diags_.error(
        expr->loc(), "a Json cannot be converted to a string: it may hold an object; check what it "
                     "holds first (e.g. 'typeof x == \"string\"'), or write it as JSON with "
                     "'JSON.stringify(x)'");
    return expr;
  }
  // An Error gives "TypeError: message", as JavaScript's Error.prototype.toString.
  if (checker_.throwChecker().isErrorType(type)) {
    return checker_.typeRelations().convert(expr, Type::string());
  }
  if (Checker::holdsObjects(type)) {
    // JavaScript would give "[object Object]", or call a toString method.
    checker_.diags_.error(expr->loc(),
                          "an object cannot be converted to a string: JavaScript would give "
                          "'[object Object]'; print it with console.log, or build the text from "
                          "its fields");
    return expr;
  }
  // A union: as the member it holds, each of which must convert (a union of
  // string literals is a string already).
  if (const Type *u = type->nonOptional(); u->isUnion() && u->isTagged()) {
    for (const Type *m : u->members()) {
      // A date, a regex and an Error have texts of their own, as alone.
      if ((m->isObject() && !m->isDate() && !m->isRegExp() &&
           !checker_.throwChecker().isErrorType(m)) ||
          m->isDict() || m->isFunction() || m->isIterator()) {
        checker_.diags_.error(expr->loc(), "a value of type '" + type->name() +
                                               "' cannot be converted to a string: it may hold '" +
                                               m->name() + "'; check what it holds first");
        return expr;
      }
    }
    auto *cast = dyn_cast<CastExpr>(checker_.typeRelations().convert(expr, Type::string()));
    if (cast) {
      cast->text = true;
    }
    return cast ? cast : expr;
  }

  // Numbers, booleans, undefined, arrays (joined with commas) and optionals of
  // those convert as in JavaScript.
  return checker_.typeRelations().convert(expr, Type::string());
}

void ConcatChecker::reportMixedConcat(const BinaryExpr *root, const BinaryExpr *mixed) {
  const Type *l = unconverted(mixed->lhs)->type, *r = unconverted(mixed->rhs)->type;
  std::string message = ("operator '+' cannot be applied to types '" + l->name() + "' and '" +
                         r->name() + "' (JavaScript would join them as text)")
                            .str();
  std::vector<std::pair<const Expr *, llvm::StringRef>> parts;
  concatParts(root, parts);
  llvm::StringRef only;
  std::string joined = concatTemplate(parts, only);
  if (joined.empty()) {
    checker_.diags_.error(mixed->loc(), message + "; use a template literal instead");
    return;
  }
  if (!only.empty()) { // `"" + n`
    checker_.diags_.error(mixed->loc(), message + "; convert it with 'String(" + only + ")'");
    return;
  }
  // `count + input` may be meant as a sum: then the string needs Number().
  if (parts.size() == 2) {
    auto [first, firstText] = parts[0];
    auto [second, secondText] = parts[1];
    auto isText = [](const Expr *e) {
      return e->type->isString() && !isa<StringLitExpr>(e) && !isa<TemplateExpr>(e);
    };
    std::string sum;
    if (first->type->isNumeric() && isText(second)) {
      sum = (firstText + " + Number(" + secondText + ")").str();
    } else if (isText(first) && second->type->isNumeric()) {
      sum = ("Number(" + firstText + ") + " + secondText).str();
    }
    if (!sum.empty()) {
      checker_.diags_.error(mixed->loc(), message + "; to add, convert the string: '" + sum +
                                              "'; to join, use a template: '" + joined + "'");
      return;
    }
  }
  checker_.diags_.error(mixed->loc(), message + "; use a template: '" + joined + "'");
}

} // namespace tz
