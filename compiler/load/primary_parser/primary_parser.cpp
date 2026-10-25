#include "compiler/load/primary_parser/primary_parser.h"
#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/literal_text/literal_text.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
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

namespace tz {

using llvm::dyn_cast;

PrimaryParser::PrimaryParser(Parser &parser) : parser_(parser) {}

Expr *PrimaryParser::parsePrimary() {
  llvm::SMLoc loc = parser_.tok_.loc();
  // `in` is an operator again inside brackets, even in a loop header.
  llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
  if (parser_.tok_.is(Tok::At)) {
    // `{ @Log m() {} }`, `@Log function () {}`: what follows is still parsed.
    parser_.classes().rejectDecorators(parser_.classes().parseDecorators());
    if (parser_.recovering_) {
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
    return parsePrimary();
  }
  switch (parser_.tok_.kind) {
  case Tok::IntLiteral:
    return parseIntLiteral();
  case Tok::FloatLiteral:
    return parseFloatLiteral();
  case Tok::StringLiteral: {
    std::u16string value;
    decodeStringLiteral(parser_.tok_, value, parser_.diags_);
    parser_.cursor().advance();
    return parser_.ctx_.make<StringLitExpr>(loc, std::move(value));
  }
  case Tok::TemplateString:
  case Tok::TemplateHead:
    return parser_.templates().parseTemplate();
  case Tok::Slash:
  case Tok::SlashAssign: {
    // An expression starts here, so `/` starts a regex literal, which the
    // lexer reads again from there (it took the `/` for division).
    if (parser_.hasPeeked_) {
      parser_.lexer_.restore(parser_.peekState_);
      parser_.hasPeeked_ = false;
    }
    parser_.tok_ = parser_.lexer_.regex(parser_.tok_.text.data(), parser_.tok_.newlineBefore);
    std::u16string pattern, flags;
    decodeRegex(parser_.tok_, pattern, flags);
    auto *lit = parser_.ctx_.make<RegexLitExpr>(loc, std::move(pattern), std::move(flags));
    lit->written = parser_.tok_.text.size();
    parser_.cursor().advance();
    return lit;
  }
  case Tok::KwTrue:
  case Tok::KwFalse: {
    bool value = parser_.tok_.is(Tok::KwTrue);
    parser_.cursor().advance();
    return parser_.ctx_.make<BoolLitExpr>(loc, value);
  }
  case Tok::Identifier: {
    if (parser_.tok_.text == "import") {
      // `import("./x")` and `import.meta`: what follows parses as a call or
      // a member of a placeholder.
      parser_.cursor().report(
          loc, parser_.cursor().peek().is(Tok::Dot)
                   ? "'import.meta' is not supported"
                   : "dynamic 'import()' is not supported: every file the program uses is "
                     "compiled into it; import by name at the top of the file, e.g. 'import "
                     "{ a } from \"./x\"'");
      parser_.cursor().advance();
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
    if (parser_.cursor().peek().is(Tok::Arrow) && !parser_.cursor().peek().newlineBefore) {
      return parser_.functions().parseArrow(); // `x => ...`
    }
    if (parser_.tok_.text == "async") {
      if (Expr *fn = parser_.functions().tryAsyncFunction()) {
        return fn;
      }
    }
    if (parser_.tok_.text == "class" &&
        (parser_.cursor().peek().is(Tok::Identifier) || parser_.cursor().peek().is(Tok::LBrace))) {
      // `const C = class { ... }`, `return class extends Base { ... }`.
      ClassDecl *cls = parser_.classes().parseClass(/*expression=*/true);
      return parser_.ctx_.make<ClassExpr>(loc, cls);
    }
    if (parser_.tok_.text == "yield" && parser_.inGenerator_) {
      // `1 + yield x` is an error in JavaScript: the operand must be in
      // parentheses.
      parser_.cursor().report(loc, "'yield' must be in parentheses here, e.g. '1 + (yield x)'");
      return parser_.exprs().parseYield();
    }
    llvm::StringRef name = parser_.tok_.text;
    parser_.cursor().advance();
    if (name == "undefined") {
      return parser_.ctx_.make<UndefinedExpr>(loc);
    }
    return parser_.ctx_.make<NameExpr>(loc, name);
  }
  case Tok::KwFunction: {
    FunctionDecl *fn = parser_.functions().parseFunction(/*expression=*/true);
    return parser_.ctx_.make<FunctionExpr>(loc, fn);
  }
  case Tok::LParen: {
    if (parser_.functions().arrowAhead()) {
      return parser_.functions().parseArrow();
    }
    parser_.cursor().advance();
    Expr *inner = parser_.exprs().parseSequence();
    parser_.cursor().expect(Tok::RParen, "to close the parenthesized expression");
    return inner;
  }
  case Tok::LBracket:
    return parseArrayLiteral();
  case Tok::LBrace:
    return parseObjectLiteral();
  case Tok::KwNew:
    return parser_.calls().parseNew();
  case Tok::Less: {
    // `<T>(x: T) => x`, a generic arrow.
    Parser::Snapshot start = parser_.cursor().snapshot();
    const char *text = parser_.tok_.text.data();
    bool muted = parser_.diags_.setMuted(true);
    parser_.recovering_ = false;
    std::vector<TypeParamDecl> typeParams = parser_.functions().parseTypeParams();
    bool arrow =
        !parser_.recovering_ && parser_.tok_.is(Tok::LParen) && parser_.functions().arrowAhead();
    parser_.diags_.setMuted(muted);
    if (arrow) {
      parser_.recovering_ = start.recovering;
      return parser_.functions().parseArrow(std::move(typeParams), text);
    }
    parser_.cursor().rewind(std::move(start));
    parser_.cursor().error(loc, "expected an expression, found '<'");
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
  default:
    parser_.cursor().error(loc, llvm::Twine("expected an expression, found ") +
                                    tokSpelling(parser_.tok_.kind));
    // Placeholder so callers never see null; the source will not compile anyway.
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
}

Expr *PrimaryParser::parseArrayLiteral() {
  auto *lit = parser_.ctx_.make<ArrayLitExpr>(parser_.tok_.loc());
  parser_.cursor().advance(); // '['
  while (!parser_.tok_.is(Tok::RBracket) && !parser_.tok_.is(Tok::Eof)) {
    if (parser_.tok_.is(Tok::Comma)) {
      // A hole: an error, unless the literal turns out to be a pattern, as in
      // `[, b] = pair`.
      lit->elements.push_back(
          {parser_.ctx_.make<UndefinedExpr>(parser_.tok_.loc()), false, /*hole=*/true});
      parser_.cursor().advance();
      continue;
    }
    bool spread = parser_.cursor().accept(Tok::Ellipsis);
    lit->elements.push_back({parser_.exprs().parseExpr(), spread});
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBracket, "to close the array");

  return lit;
}

// `{ x: 1, y }`
Expr *PrimaryParser::parseObjectLiteral() {
  auto *lit = parser_.ctx_.make<ObjectLitExpr>(parser_.tok_.loc());
  parser_.cursor().advance(); // '{'
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    // `{ @Log m() {} }`: what follows is still parsed.
    if (parser_.tok_.is(Tok::At)) {
      parser_.classes().rejectDecorators(parser_.classes().parseDecorators());
      if (parser_.recovering_) {
        return lit;
      }
    }
    llvm::SMLoc loc = parser_.tok_.loc();
    if (parser_.cursor().accept(Tok::Ellipsis)) {
      // `{ ...o }` copies the fields of o.
      ObjectLitExpr::Property spread;
      spread.loc = loc;
      spread.value = parser_.exprs().parseExpr();
      spread.spread = true;
      lit->props.push_back(std::move(spread));
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    if (!parser_.classes().symbolKeyAhead() && parser_.cursor().accept(Tok::LBracket)) {
      // `[key]: value`, where the key must be a constant (see Checker).
      ObjectLitExpr::Property prop;
      prop.loc = loc;
      prop.key = parser_.exprs().parseExpr();
      parser_.cursor().expect(Tok::RBracket, "to close the key");
      // The field it names, if the key is known by now (see fieldNames).
      if (auto *name = dyn_cast<NameExpr>(prop.key)) {
        for (const std::string &text : parser_.stringConstants_.lookup(name->name)) {
          parser_.fieldNames_.insert(text);
        }
      } else if (auto *text = dyn_cast<StringLitExpr>(prop.key)) {
        parser_.fieldNames_.insert(toUtf8(text->value));
      }
      if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
        parser_.cursor().error(
            loc, "a method cannot have a key in brackets; write its name: 'f() { ... }'");
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      if (!parser_.cursor().expect(Tok::Colon, "after the key in brackets")) {
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      prop.value = parser_.exprs().parseExpr();
      lit->props.push_back(std::move(prop));
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    // `get x() {}`, `set x(v) {}`, `async f() {}`
    bool accessor =
        parser_.tok_.is(Tok::Identifier) &&
        (parser_.tok_.text == "get" || parser_.tok_.text == "set") &&
        (parser_.cursor().peek().is(Tok::Identifier) || isKeyword(parser_.cursor().peek().kind) ||
         parser_.cursor().peek().is(Tok::StringLiteral));
    if (accessor) {
      parser_.cursor().error(
          loc, "getters and setters are not supported: reading or writing a field never runs code; "
               "write a function instead");
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    // `async f() {}`
    bool isAsync =
        parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "async" &&
        !parser_.cursor().peek().is(Tok::Colon) && !parser_.cursor().peek().is(Tok::Comma) &&
        !parser_.cursor().peek().is(Tok::RBrace) && !parser_.cursor().peek().is(Tok::LParen) &&
        !parser_.cursor().peek().newlineBefore;
    if (isAsync) {
      parser_.cursor().advance();
    }
    const char *start = parser_.tok_.text.data();
    bool generator = parser_.cursor().accept(Tok::Star); // `*items() { yield ... }`
    bool identifier = parser_.tok_.is(Tok::Identifier);
    llvm::StringRef name;
    if (!parser_.objectTypes().parseFieldName(name)) {
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    Expr *value = nullptr;
    bool defaulted = false, method = false;
    if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
      // `f() { ... }`: a method, which sees the object as `this`.
      auto *fn = parser_.ctx_.make<FunctionDecl>(loc, name);
      fn->isExpression = true;
      fn->isGenerator = generator;
      fn->isAsync = isAsync;
      parser_.classes().parseMethodRest(fn, start);
      if (parser_.recovering_ || !fn->body) {
        if (!fn->body) {
          parser_.cursor().error(parser_.tok_.loc(),
                                 "expected '{' to start the body of the method");
        }
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      value = parser_.ctx_.make<FunctionExpr>(loc, fn);
      method = true;
    } else if (generator || isAsync) {
      parser_.cursor().error(parser_.tok_.loc(),
                             generator ? "expected '(' after the name of the generator method"
                                       : "expected '(' after the name of the async method");
      parser_.cursor().skipToClosingBrace();
      return lit;
    } else if (parser_.cursor().accept(Tok::Colon)) {
      value = parser_.exprs().parseExpr();
    } else if (identifier && (parser_.tok_.is(Tok::Comma) || parser_.tok_.is(Tok::RBrace))) {
      // `{ x }` is `{ x: x }`.
      value = name == "undefined" ? static_cast<Expr *>(parser_.ctx_.make<UndefinedExpr>(loc))
                                  : parser_.ctx_.make<NameExpr>(loc, name);
    } else if (identifier && parser_.tok_.is(Tok::Assign)) {
      // `{ x = 1 }`: a default value, which only a destructuring pattern
      // such as `({ x = 1 } = o)` may have (Checker reports it otherwise).
      llvm::SMLoc assignLoc = parser_.tok_.loc();
      parser_.cursor().advance();
      value = parser_.ctx_.make<AssignExpr>(assignLoc, std::nullopt,
                                            parser_.ctx_.make<NameExpr>(loc, name),
                                            parser_.exprs().parseExpr());
      defaulted = true;
    } else {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected ':' after the field name, found ") +
                                 tokSpelling(parser_.tok_.kind));
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    ObjectLitExpr::Property prop;
    prop.name = name;
    prop.loc = loc;
    prop.value = value;
    prop.defaulted = defaulted;
    prop.method = method;
    lit->props.push_back(std::move(prop));
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the object");

  return lit;
}

Expr *PrimaryParser::parseIntLiteral() {
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef source = parser_.tok_.text;
  llvm::SmallString<32> digits;
  for (char c : parser_.tok_.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  parser_.cursor().advance();

  llvm::StringRef text = digits;
  unsigned radix = 10;
  if (text.size() > 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x':
    case 'X':
      radix = 16;
      break;
    case 'b':
    case 'B':
      radix = 2;
      break;
    case 'o':
    case 'O':
      radix = 8;
      break;
    default:
      break;
    }
    if (radix != 10) {
      text = text.drop_front(2);
    }
  }

  uint64_t value = 0;
  if (!text.getAsInteger(radix, value)) {
    auto *lit = parser_.ctx_.make<IntLitExpr>(loc, value);
    lit->text = source; // a decimal reads its value from the text
    return lit;
  }
  for (char c : text) {
    unsigned digit = llvm::isDigit(c) ? c - '0' : llvm::isAlpha(c) ? (c | 0x20) - 'a' + 10 : radix;
    if (digit >= radix) {
      parser_.cursor().report(loc, "invalid integer literal");
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
  }
  // Beyond 64 bits: the nearest double, as in JavaScript.
  double rounded = 0;
  if (radix == 10) {
    text.getAsDouble(rounded);
  } else {
    unsigned bits = static_cast<unsigned>(text.size()) * (radix == 16 ? 4 : radix == 8 ? 3 : 1);
    llvm::APFloat number(llvm::APFloat::IEEEdouble());
    number.convertFromAPInt(llvm::APInt(bits, text, static_cast<uint8_t>(radix)),
                            /*IsSigned=*/false, llvm::APFloat::rmNearestTiesToEven);
    rounded = number.convertToDouble();
  }
  auto *lit = parser_.ctx_.make<FloatLitExpr>(loc, rounded);
  lit->text = source;

  return lit;
}

Expr *PrimaryParser::parseFloatLiteral() {
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef text = parser_.tok_.text;
  llvm::SmallString<32> digits;
  for (char c : parser_.tok_.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  parser_.cursor().advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value)) {
    parser_.cursor().report(loc, "invalid number literal");
  }
  auto *lit = parser_.ctx_.make<FloatLitExpr>(loc, value);
  lit->text = text;

  return lit;
}

} // namespace tz
