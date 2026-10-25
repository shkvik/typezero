#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

PatternParser::PatternParser(Parser &parser) : parser_(parser) {}

// `[a, , b = 1, [c, d], ...rest]` in a declaration.
std::unique_ptr<Parser::Pattern> PatternParser::parseBindingPattern() {
  if (parser_.tok_.is(Tok::LBrace)) {
    return parseObjectPattern();
  }
  auto pattern = std::make_unique<Parser::Pattern>();
  pattern->loc = parser_.tok_.loc();
  parser_.cursor().advance(); // '['
  while (!parser_.tok_.is(Tok::RBracket) && !parser_.tok_.is(Tok::Eof)) {
    Parser::Pattern::Element e;
    e.loc = parser_.tok_.loc();
    if (parser_.cursor().accept(Tok::Comma)) {
      e.hole = true;
      pattern->elements.push_back(std::move(e));
      continue;
    }
    e.rest = parser_.cursor().accept(Tok::Ellipsis);
    if (parser_.tok_.is(Tok::LBracket) || parser_.tok_.is(Tok::LBrace)) {
      e.nested = parseBindingPattern();
    } else if (parser_.tok_.is(Tok::Identifier)) {
      e.name = parser_.tok_.text;
      parser_.cursor().advance();
    } else {
      parser_.cursor().error(
          parser_.tok_.loc(),
          llvm::Twine("expected a variable name in the destructuring pattern, found ") +
              tokSpelling(parser_.tok_.kind));
      return pattern;
    }
    if (parser_.tok_.is(Tok::Assign)) {
      if (e.rest) {
        parser_.cursor().report(parser_.tok_.loc(), "a rest element cannot have a default value");
      }
      parser_.cursor().advance();
      e.defaultValue = parser_.exprs().parseExpr();
    }
    bool rest = e.rest;
    pattern->elements.push_back(std::move(e));
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
    if (rest) {
      parser_.cursor().report(pattern->elements.back().loc, "a rest element must be the last one");
    }
  }
  parser_.cursor().expect(Tok::RBracket, "to close the destructuring pattern");

  return pattern;
}

// `{ a, b: c, d = 1, e: { f }, "g-h": g, [key]: k, ...rest }` in a
// declaration.
std::unique_ptr<Parser::Pattern> PatternParser::parseObjectPattern() {
  auto pattern = std::make_unique<Parser::Pattern>();
  pattern->loc = parser_.tok_.loc();
  pattern->object = true;
  parser_.cursor().advance(); // '{'
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    Parser::Pattern::Element e;
    e.loc = parser_.tok_.loc();
    if (parser_.cursor().accept(Tok::Ellipsis)) {
      e.rest = true;
      if (!parser_.tok_.is(Tok::Identifier)) {
        parser_.cursor().error(parser_.tok_.loc(),
                               "expected a variable name after '...' in the destructuring pattern");
        return pattern;
      }
      e.name = parser_.tok_.text;
      parser_.cursor().advance();
    } else {
      bool identifier = parser_.tok_.is(Tok::Identifier);
      if (parser_.cursor().accept(Tok::LBracket)) {
        e.keyExpr = parser_.exprs().parseExpr();
        parser_.cursor().expect(Tok::RBracket, "to close the key");
      } else if (!parser_.objectTypes().parseFieldName(e.key)) {
        parser_.cursor().skipToClosingBrace();
        return pattern;
      }
      if (parser_.cursor().accept(Tok::Colon)) {
        if (parser_.tok_.is(Tok::LBracket) || parser_.tok_.is(Tok::LBrace)) {
          e.nested = parseBindingPattern();
        } else if (parser_.tok_.is(Tok::Identifier)) {
          e.name = parser_.tok_.text;
          parser_.cursor().advance();
        } else {
          parser_.cursor().error(
              parser_.tok_.loc(),
              llvm::Twine("expected a variable name in the destructuring pattern, found ") +
                  tokSpelling(parser_.tok_.kind));
          return pattern;
        }
      } else if (identifier && !e.keyExpr) {
        e.name = e.key; // `{ x }` is `{ x: x }`
      } else {
        parser_.cursor().error(parser_.tok_.loc(),
                               llvm::Twine("a key in a destructuring pattern needs a variable: '") +
                                   (e.keyExpr ? llvm::Twine("[key]") : llvm::Twine(e.key)) +
                                   ": name'");
        return pattern;
      }
      if (parser_.cursor().accept(Tok::Assign)) {
        e.defaultValue = parser_.exprs().parseExpr();
      }
    }
    bool rest = e.rest;
    pattern->elements.push_back(std::move(e));
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
    if (rest) {
      parser_.cursor().report(pattern->elements.back().loc, "a rest element must be the last one");
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the destructuring pattern");

  return pattern;
}

std::unique_ptr<Parser::Pattern> PatternParser::patternFromLiteral(Expr *expr) {
  auto pattern = std::make_unique<Parser::Pattern>();
  pattern->loc = expr->loc();
  if (auto *object = dyn_cast<ObjectLitExpr>(expr)) {
    // `({ a, b: o.c, d = 1, ...rest } = value)`
    pattern->object = true;
    for (size_t i = 0; i < object->props.size(); ++i) {
      const ObjectLitExpr::Property &prop = object->props[i];
      Parser::Pattern::Element e;
      e.loc = prop.loc;
      e.rest = prop.spread;
      e.key = prop.name;
      e.keyExpr = prop.key;
      if (e.rest && i + 1 != object->props.size()) {
        parser_.cursor().report(e.loc, "a rest element must be the last one");
      }
      Expr *target = prop.value;
      if (auto *assign = dyn_cast<AssignExpr>(target); assign && !assign->op && !e.rest) {
        target = assign->target; // `{ a = 1 }` or `{ a: b = 1 }`
        e.defaultValue = assign->value;
      }
      if (isa<ArrayLitExpr>(target) || isa<ObjectLitExpr>(target)) {
        e.nested = patternFromLiteral(target);
      } else {
        e.target = target;
      }
      pattern->elements.push_back(std::move(e));
    }
    return pattern;
  }
  auto *lit = cast<ArrayLitExpr>(expr);
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    const ArrayLitExpr::Element &element = lit->elements[i];
    Parser::Pattern::Element e;
    e.loc = element.expr->loc();
    e.hole = element.hole;
    e.rest = element.spread;
    if (e.rest && i + 1 != lit->elements.size()) {
      parser_.cursor().report(e.loc, "a rest element must be the last one");
    }
    Expr *target = element.expr;
    if (auto *assign = dyn_cast<AssignExpr>(target); assign && !assign->op && !e.rest) {
      target = assign->target; // `[a = 1] = values`
      e.defaultValue = assign->value;
    }
    if (isa<ArrayLitExpr>(target) || isa<ObjectLitExpr>(target)) {
      e.nested = patternFromLiteral(target);
    } else if (!e.hole) {
      e.target = target;
    }
    pattern->elements.push_back(std::move(e));
  }

  return pattern;
}

VarDeclarator PatternParser::hiddenDeclarator(llvm::SMLoc loc, Expr *init) {
  VarDeclarator d;
  d.name = parser_.ctx_.hiddenName();
  d.loc = loc;
  d.init = init;

  return d;
}

// `const [a, b = 1, ...rest] = value` is
// `const %0 = value, a = %0[0], b = %0[1] ?? 1, rest = %0.slice(2)`.
void PatternParser::declarePattern(VarDeclStmt *decl, const Parser::Pattern &pattern, Expr *value,
                                   std::optional<TypeRef> typeAnn) {
  VarDeclarator hidden = hiddenDeclarator(pattern.loc, value);
  hidden.typeAnn = std::move(typeAnn);
  hidden.destructured = true;
  hidden.objectPattern = pattern.object;
  hidden.patternLength = pattern.elements.size();
  hidden.patternRest = !pattern.elements.empty() && pattern.elements.back().rest;
  decl->decls.push_back(hidden);
  declareElements(decl, pattern, hidden.name);
}

void PatternParser::declareElements(VarDeclStmt *decl, const Parser::Pattern &pattern,
                                    llvm::StringRef base) {
  std::vector<const IndexExpr *> keyReads;
  for (size_t i = 0; i < pattern.elements.size(); ++i) {
    const Parser::Pattern::Element &e = pattern.elements[i];
    if (e.hole) {
      continue;
    }
    Expr *value = elementOf(base, pattern, i, keyReads);
    if (e.nested) {
      declarePattern(decl, *e.nested, value, std::nullopt);
      continue;
    }
    VarDeclarator d;
    d.name = e.name;
    d.loc = e.loc;
    d.init = value;
    decl->decls.push_back(d);
  }
}

// `[a, b] = value` is `{ const %0 = value; a = %0[0]; b = %0[1] }`. When value
// is an array literal, as in `[a, b] = [b, a]`, its elements go to hidden
// variables instead, and no array is made: `{ const %0 = b, %1 = a; a = %0;
// b = %1 }`. The same for an object pattern, `({ a, b } = o)`, with fields.
Stmt *PatternParser::assignPattern(llvm::SMLoc loc, const Parser::Pattern &pattern, Expr *value) {
  auto *block = parser_.ctx_.make<BlockStmt>(loc);
  auto *decl = parser_.ctx_.make<VarDeclStmt>(loc, /*isConst=*/true);
  block->body.push_back(decl);
  auto *lit = dyn_cast<ArrayLitExpr>(value);
  bool direct =
      !pattern.object && lit && lit->elements.size() >= pattern.elements.size() &&
      llvm::none_of(lit->elements,
                    [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; }) &&
      llvm::none_of(pattern.elements, [](const Parser::Pattern::Element &e) { return e.rest; });
  std::vector<llvm::StringRef> values;
  if (direct) {
    for (const ArrayLitExpr::Element &e : lit->elements) {
      decl->decls.push_back(hiddenDeclarator(e.expr->loc(), e.expr));
      values.push_back(decl->decls.back().name);
    }
  } else {
    VarDeclarator hidden = hiddenDeclarator(pattern.loc, value);
    hidden.destructured = true;
    hidden.objectPattern = pattern.object;
    hidden.patternLength = pattern.elements.size();
    hidden.patternRest = !pattern.elements.empty() && pattern.elements.back().rest;
    decl->decls.push_back(hidden);
  }
  llvm::StringRef base = decl->decls.front().name;
  std::vector<const IndexExpr *> keyReads;
  for (size_t i = 0; i < pattern.elements.size(); ++i) {
    const Parser::Pattern::Element &e = pattern.elements[i];
    if (e.hole) {
      continue;
    }
    Expr *element;
    if (direct) {
      element = parser_.ctx_.make<NameExpr>(e.loc, values[i]);
      if (e.defaultValue) {
        element = parser_.ctx_.make<BinaryExpr>(e.loc, BinaryOp::Nullish, element, e.defaultValue);
      }
    } else {
      element = elementOf(base, pattern, i, keyReads);
    }
    if (e.nested) {
      block->body.push_back(assignPattern(e.loc, *e.nested, element));
    } else {
      block->body.push_back(parser_.ctx_.make<ExprStmt>(
          e.loc, parser_.ctx_.make<AssignExpr>(e.loc, std::nullopt, e.target, element)));
    }
  }

  return block;
}

Expr *PatternParser::elementOf(llvm::StringRef base, const Parser::Pattern &pattern, size_t i,
                               std::vector<const IndexExpr *> &keyReads) {
  const Parser::Pattern::Element &e = pattern.elements[i];
  auto *source = parser_.ctx_.make<NameExpr>(e.loc, base);
  Expr *element;
  if (!pattern.object) {
    auto *position = parser_.ctx_.make<IntLitExpr>(e.loc, i);
    if (e.rest) {
      auto *slice = parser_.ctx_.make<MemberExpr>(e.loc, source, "slice");
      return parser_.ctx_.make<CallExpr>(e.loc, slice, std::vector<Expr *>{position});
    }
    element = parser_.ctx_.make<IndexExpr>(e.loc, source, position);
  } else if (e.rest) {
    // `{ a, ...rest }`: a new object with the other fields, `{ ...%0 }`
    // without a.
    auto *copy = parser_.ctx_.make<ObjectLitExpr>(e.loc);
    ObjectLitExpr::Property spread;
    spread.loc = e.loc;
    spread.value = source;
    spread.spread = true;
    for (const Parser::Pattern::Element &other : pattern.elements) {
      if (!other.rest && !other.keyExpr) {
        spread.omit.push_back(other.key);
      }
    }
    spread.omitKeys = keyReads;
    copy->props.push_back(std::move(spread));
    return copy;
  } else if (e.keyExpr) {
    auto *read = parser_.ctx_.make<IndexExpr>(e.loc, source, e.keyExpr);
    keyReads.push_back(read);
    element = read;
  } else {
    element = parser_.ctx_.make<MemberExpr>(e.loc, source, e.key);
  }
  if (e.defaultValue) {
    element = parser_.ctx_.make<BinaryExpr>(e.loc, BinaryOp::Nullish, element, e.defaultValue);
  }

  return element;
}

} // namespace tz
