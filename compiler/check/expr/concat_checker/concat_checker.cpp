#include "compiler/check/expr/concat_checker/concat_checker.h"
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

std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source, llvm::SMLoc op,
                                                         size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

const Expr *unconverted(const Expr *expr) {
  auto *cast = dyn_cast<CastExpr>(expr);

  return cast && cast->isImplicit() && cast->type->isString() ? cast->operand : expr;
}

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
  if (values != 1 || result.size() != only.size() + 4) {
    only = "";
  }

  return result + "`";
}

}

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
  if (type->isDate()) {
    return checker_.dateChecker().dateToString(expr);
  }
  if (type->isBytes()) {
    return checker_.bytesChecker().bytesToString(expr);
  }
  if (type->nonOptional()->isParam()) {
    checker_.genericChecker().noteForced(expr, Type::string());
    return checker_.typeRelations().convert(expr, Type::string());
  }
  if (Checker::holdsFunctions(type)) {
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
      cls && !ClassDeclarer::methodsNamed(cls, "toString", false).empty()) {
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
    checker_.diags_.error(
        expr->loc(), "a Json cannot be converted to a string: it may hold an object; check what it "
                     "holds first (e.g. 'typeof x == \"string\"'), or write it as JSON with "
                     "'JSON.stringify(x)'");
    return expr;
  }
  if (Checker::holdsObjects(type)) {
    checker_.diags_.error(expr->loc(),
                          "an object cannot be converted to a string: JavaScript would give "
                          "'[object Object]'; print it with console.log, or build the text from "
                          "its fields");
    return expr;
  }
  if (const Type *u = type->nonOptional(); u->isUnion() && u->isTagged()) {
    for (const Type *m : u->members()) {
      if ((m->isObject() && !m->isDate() && !m->isRegExp()) || m->isDict() || m->isFunction() ||
          m->isIterator()) {
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
  if (!only.empty()) {
    checker_.diags_.error(mixed->loc(), message + "; convert it with 'String(" + only + ")'");
    return;
  }
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

}
