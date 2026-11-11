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
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

using llvm::dyn_cast;

PrimaryParser::PrimaryParser(Parser &parser)
    : parser(parser) {}

Expr *PrimaryParser::parsePrimary() {
  llvm::SMLoc loc = this->parser.tok.loc();
  llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
  if (this->parser.tok.is(Tok::At)) {
    this->parser.classes().rejectDecorators(this->parser.classes().parseDecorators());
    if (this->parser.recovering) {
      return this->parser.ctx.make<IntLitExpr>(loc, 0);
    }
    return parsePrimary();
  }
  switch (this->parser.tok.kind) {
    case Tok::IntLiteral:
      return parseIntLiteral();
    case Tok::FloatLiteral:
      return parseFloatLiteral();
    case Tok::StringLiteral: {
      std::u16string value;
      LiteralText::decodeStringLiteral(this->parser.tok, value, this->parser.diags);
      this->parser.cursor().advance();
      return this->parser.ctx.make<StringLitExpr>(loc, std::move(value));
    }
    case Tok::TemplateString:
    case Tok::TemplateHead:
      return this->parser.templates().parseTemplate();
    case Tok::Slash:
    case Tok::SlashAssign: {
      if (this->parser.hasPeeked) {
        this->parser.lexer.restore(this->parser.peekState);
        this->parser.hasPeeked = false;
      }
      this->parser.tok =
          this->parser.lexer.regex(this->parser.tok.text.data(), this->parser.tok.newlineBefore);
      std::u16string pattern, flags;
      LiteralText::decodeRegex(this->parser.tok, pattern, flags);
      auto *lit = this->parser.ctx.make<RegexLitExpr>(loc, std::move(pattern), std::move(flags));
      lit->written = this->parser.tok.text.size();
      this->parser.cursor().advance();
      return lit;
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      bool value = this->parser.tok.is(Tok::KwTrue);
      this->parser.cursor().advance();
      return this->parser.ctx.make<BoolLitExpr>(loc, value);
    }
    case Tok::Identifier: {
      if (this->parser.tok.text == "import") {
        this->parser.cursor().report(
            loc, this->parser.cursor().peek().is(Tok::Dot)
                     ? "'import.meta' is not supported"
                     : "dynamic 'import()' is not supported: every file the program uses is "
                       "compiled into it; import by name at the top of the file, e.g. 'import "
                       "{ a } from \"./x\"'");
        this->parser.cursor().advance();
        return this->parser.ctx.make<IntLitExpr>(loc, 0);
      }
      if (this->parser.cursor().peek().is(Tok::Arrow) &&
          !this->parser.cursor().peek().newlineBefore) {
        return this->parser.functions().parseArrow();
      }
      if (this->parser.tok.text == "async") {
        if (Expr *fn = this->parser.functions().tryAsyncFunction()) {
          return fn;
        }
      }
      if (this->parser.tok.text == "class" && (this->parser.cursor().peek().is(Tok::Identifier) ||
                                               this->parser.cursor().peek().is(Tok::LBrace))) {
        ClassDecl *cls = this->parser.classes().parseClass(true);
        return this->parser.ctx.make<ClassExpr>(loc, cls);
      }
      if (this->parser.tok.text == "yield" && this->parser.inGenerator) {
        this->parser.cursor().report(loc,
                                     "'yield' must be in parentheses here, e.g. '1 + (yield x)'");
        return this->parser.exprs().parseYield();
      }
      llvm::StringRef name = this->parser.tok.text;
      this->parser.cursor().advance();
      if (name == "undefined") {
        return this->parser.ctx.make<UndefinedExpr>(loc);
      }
      return this->parser.ctx.make<NameExpr>(loc, name);
    }
    case Tok::KwFunction: {
      FunctionDecl *fn = this->parser.functions().parseFunction(true);
      return this->parser.ctx.make<FunctionExpr>(loc, fn);
    }
    case Tok::LParen: {
      if (this->parser.functions().arrowAhead()) {
        return this->parser.functions().parseArrow();
      }
      this->parser.cursor().advance();
      Expr *inner = this->parser.exprs().parseSequence();
      this->parser.cursor().expect(Tok::RParen, "to close the parenthesized expression");
      return inner;
    }
    case Tok::LBracket:
      return parseArrayLiteral();
    case Tok::LBrace:
      return parseObjectLiteral();
    case Tok::KwNew:
      return this->parser.calls().parseNew();
    case Tok::Less: {
      Parser::Snapshot start = this->parser.cursor().snapshot();
      const char *text = this->parser.tok.text.data();
      this->parser.diags.pushList(DiagnosticRoute::DISCARD);
      this->parser.recovering = false;
      std::vector<TypeParamDecl> typeParams = this->parser.functions().parseTypeParams();
      bool arrow = !this->parser.recovering && this->parser.tok.is(Tok::LParen) &&
                   this->parser.functions().arrowAhead();
      this->parser.diags.popList();
      if (arrow) {
        this->parser.recovering = start.recovering;
        return this->parser.functions().parseArrow(std::move(typeParams), text);
      }
      this->parser.cursor().rewind(std::move(start));
      this->parser.cursor().error(loc, "expected an expression, found '<'");
      return this->parser.ctx.make<IntLitExpr>(loc, 0);
    }
    default:
      this->parser.cursor().error(loc, llvm::Twine("expected an expression, found ") +
                                           TokenKinds::tokSpelling(this->parser.tok.kind));
      return this->parser.ctx.make<IntLitExpr>(loc, 0);
  }
}

Expr *PrimaryParser::parseArrayLiteral() {
  auto *lit = this->parser.ctx.make<ArrayLitExpr>(this->parser.tok.loc());
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::RBracket) && !this->parser.tok.is(Tok::Eof)) {
    if (this->parser.tok.is(Tok::Comma)) {
      lit->elements.push_back(
          {this->parser.ctx.make<UndefinedExpr>(this->parser.tok.loc()), false, true});
      this->parser.cursor().advance();
      continue;
    }
    bool spread = this->parser.cursor().accept(Tok::Ellipsis);
    lit->elements.push_back({this->parser.exprs().parseExpr(), spread});
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBracket, "to close the array");

  return lit;
}

Expr *PrimaryParser::parseObjectLiteral() {
  auto *lit = this->parser.ctx.make<ObjectLitExpr>(this->parser.tok.loc());
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    if (this->parser.tok.is(Tok::At)) {
      this->parser.classes().rejectDecorators(this->parser.classes().parseDecorators());
      if (this->parser.recovering) {
        return lit;
      }
    }
    llvm::SMLoc loc = this->parser.tok.loc();
    if (this->parser.cursor().accept(Tok::Ellipsis)) {
      ObjectLitExpr::Property spread;
      spread.loc = loc;
      spread.value = this->parser.exprs().parseExpr();
      spread.form = SpreadProperty{};
      lit->props.push_back(std::move(spread));
      if (!this->parser.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    if (!this->parser.classes().symbolKeyAhead() && this->parser.cursor().accept(Tok::LBracket)) {
      ObjectLitExpr::Property prop;
      prop.loc = loc;
      Expr *key = this->parser.exprs().parseExpr();
      prop.form = ComputedProperty{.key = key};
      this->parser.cursor().expect(Tok::RBracket, "to close the key");
      if (auto *name = dyn_cast<NameExpr>(prop.key())) {
        for (const std::string &text : this->parser.stringConstants.lookup(name->name)) {
          this->parser.fieldNames.insert(text);
        }
      } else if (auto *text = dyn_cast<StringLitExpr>(prop.key())) {
        this->parser.fieldNames.insert(ToUtf8::toUtf8(text->value));
      }
      if (this->parser.tok.is(Tok::LParen) || this->parser.tok.is(Tok::Less)) {
        this->parser.cursor().error(
            loc, "a method cannot have a key in brackets; write its name: 'f() { ... }'");
        this->parser.cursor().skipToClosingBrace();
        return lit;
      }
      if (!this->parser.cursor().expect(Tok::Colon, "after the key in brackets")) {
        this->parser.cursor().skipToClosingBrace();
        return lit;
      }
      prop.value = this->parser.exprs().parseExpr();
      lit->props.push_back(std::move(prop));
      if (!this->parser.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    bool accessor = this->parser.tok.is(Tok::Identifier) &&
                    (this->parser.tok.text == "get" || this->parser.tok.text == "set") &&
                    (this->parser.cursor().peek().is(Tok::Identifier) ||
                     TokenKinds::isKeyword(this->parser.cursor().peek().kind) ||
                     this->parser.cursor().peek().is(Tok::StringLiteral));
    if (accessor) {
      this->parser.cursor().error(
          loc, "getters and setters are not supported: reading or writing a field never runs code; "
               "write a function instead");
      this->parser.cursor().skipToClosingBrace();
      return lit;
    }
    bool isAsync = this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "async" &&
                   !this->parser.cursor().peek().is(Tok::Colon) &&
                   !this->parser.cursor().peek().is(Tok::Comma) &&
                   !this->parser.cursor().peek().is(Tok::RBrace) &&
                   !this->parser.cursor().peek().is(Tok::LParen) &&
                   !this->parser.cursor().peek().newlineBefore;
    if (isAsync) {
      this->parser.cursor().advance();
    }
    const char *start = this->parser.tok.text.data();
    bool generator = this->parser.cursor().accept(Tok::Star);
    bool identifier = this->parser.tok.is(Tok::Identifier);
    llvm::StringRef name;
    if (!this->parser.objectTypes().parseFieldName(name)) {
      this->parser.cursor().skipToClosingBrace();
      return lit;
    }
    Expr *value = nullptr;
    PropertyForm form = ValueProperty{};
    if (this->parser.tok.is(Tok::LParen) || this->parser.tok.is(Tok::Less)) {
      auto *fn = this->parser.ctx.make<FunctionDecl>(loc, name);
      fn->place = FunctionPlace::EXPRESSION;
      fn->isGenerator = generator;
      fn->isAsync = isAsync;
      this->parser.classes().parseMethodRest(fn, start);
      if (this->parser.recovering || !fn->body) {
        if (!fn->body) {
          this->parser.cursor().error(this->parser.tok.loc(),
                                      "expected '{' to start the body of the method");
        }
        this->parser.cursor().skipToClosingBrace();
        return lit;
      }
      value = this->parser.ctx.make<FunctionExpr>(loc, fn);
      form = MethodProperty{};
    } else if (generator || isAsync) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  generator ? "expected '(' after the name of the generator method"
                                            : "expected '(' after the name of the async method");
      this->parser.cursor().skipToClosingBrace();
      return lit;
    } else if (this->parser.cursor().accept(Tok::Colon)) {
      value = this->parser.exprs().parseExpr();
    } else if (identifier &&
               (this->parser.tok.is(Tok::Comma) || this->parser.tok.is(Tok::RBrace))) {
      value = name == "undefined" ? static_cast<Expr *>(this->parser.ctx.make<UndefinedExpr>(loc))
                                  : this->parser.ctx.make<NameExpr>(loc, name);
    } else if (identifier && this->parser.tok.is(Tok::Assign)) {
      llvm::SMLoc assignLoc = this->parser.tok.loc();
      this->parser.cursor().advance();
      value = this->parser.ctx.make<AssignExpr>(assignLoc, std::nullopt,
                                                this->parser.ctx.make<NameExpr>(loc, name),
                                                this->parser.exprs().parseExpr());
      form = DefaultedProperty{};
    } else {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected ':' after the field name, found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      this->parser.cursor().skipToClosingBrace();
      return lit;
    }
    ObjectLitExpr::Property prop;
    prop.name = name;
    prop.loc = loc;
    prop.value = value;
    prop.form = std::move(form);
    lit->props.push_back(std::move(prop));
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, "to close the object");

  return lit;
}

Expr *PrimaryParser::parseIntLiteral() {
  llvm::SMLoc loc = this->parser.tok.loc();
  llvm::StringRef source = this->parser.tok.text;
  llvm::SmallString<32> digits;
  for (char c : this->parser.tok.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  this->parser.cursor().advance();

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
    auto *lit = this->parser.ctx.make<IntLitExpr>(loc, value);
    lit->text = source;
    return lit;
  }
  for (char c : text) {
    unsigned digit = llvm::isDigit(c) ? c - '0' : llvm::isAlpha(c) ? (c | 0x20) - 'a' + 10 : radix;
    if (digit >= radix) {
      this->parser.cursor().report(loc, "invalid integer literal");
      return this->parser.ctx.make<IntLitExpr>(loc, 0);
    }
  }
  double rounded = 0;
  if (radix == 10) {
    text.getAsDouble(rounded);
  } else {
    unsigned bits = static_cast<unsigned>(text.size()) * (radix == 16 ? 4 : radix == 8 ? 3 : 1);
    llvm::APFloat number(llvm::APFloat::IEEEdouble());
    number.convertFromAPInt(llvm::APInt(bits, text, static_cast<uint8_t>(radix)), false,
                            llvm::APFloat::rmNearestTiesToEven);
    rounded = number.convertToDouble();
  }
  auto *lit = this->parser.ctx.make<FloatLitExpr>(loc, rounded);
  lit->text = source;

  return lit;
}

Expr *PrimaryParser::parseFloatLiteral() {
  llvm::SMLoc loc = this->parser.tok.loc();
  llvm::StringRef text = this->parser.tok.text;
  llvm::SmallString<32> digits;
  for (char c : this->parser.tok.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  this->parser.cursor().advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value)) {
    this->parser.cursor().report(loc, "invalid number literal");
  }
  auto *lit = this->parser.ctx.make<FloatLitExpr>(loc, value);
  lit->text = text;

  return lit;
}

}
