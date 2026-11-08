#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/module_parser/module_parser.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FunctionParser::FunctionParser(Parser &parser)
    : parser(parser) {}

Stmt *FunctionParser::parseFunctionDecl(bool isAsync) {
  FunctionDecl *fn = parseFunction(false, isAsync);
  if (fn->body) {
    return fn;
  }
  llvm::StringRef name = fn->name;
  llvm::SMLoc loc = fn->loc();
  while (!fn->body) {
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "export" &&
        this->parser.cursor().peek().is(Tok::KwFunction)) {
      this->parser.cursor().advance();
    }
    if (!this->parser.tok.is(Tok::KwFunction) || this->parser.cursor().peek().text != name) {
      this->parser.cursor().report(
          loc, "function '" + name + "' has no body; add it after the parameters: '{ ... }'");
      return this->parser.ctx.make<EmptyStmt>(loc);
    }
    fn = parseFunction(false, isAsync);
  }
  this->parser.cursor().report(
      loc, "overload signatures are not supported: plain functions are not overloaded; give each "
           "variant its own name and body");

  return fn;
}

Stmt *FunctionParser::parseDeclaredFunction() {
  FunctionDecl *fn = parseFunction(false, false);
  if (fn->body) {
    this->parser.cursor().report(fn->loc(),
                                 "a declared function has no body: the runtime of tz has "
                                 "its code");
    return fn;
  }
  fn->bodySource = FunctionBodySource::EXTERNAL;
  fn->body = this->parser.ctx.make<BlockStmt>(fn->loc());

  return fn;
}

FunctionDecl *FunctionParser::parseFunction(bool expression, bool isAsync) {
  const char *start = this->parser.tok.text.data();
  llvm::SMLoc keywordLoc = this->parser.tok.loc();
  this->parser.cursor().advance();
  bool generator = this->parser.cursor().accept(Tok::Star);

  llvm::StringRef name;
  llvm::SMLoc nameLoc = this->parser.tok.loc();
  if (this->parser.tok.is(Tok::Identifier)) {
    name = this->parser.tok.text;
    this->parser.cursor().advance();
  } else if (expression) {
    nameLoc = keywordLoc;
  } else {
    this->parser.cursor().error(this->parser.tok.loc(), "expected a function name");
  }
  auto *fn = this->parser.ctx.make<FunctionDecl>(nameLoc, name);
  fn->place = expression ? FunctionPlace::EXPRESSION : FunctionPlace::DECLARED;
  fn->isGenerator = generator;
  fn->isAsync = isAsync;
  if (expression) {
    fn->syntax = RegularFunction{.ownName = name};
  }
  if (this->parser.tok.is(Tok::Less)) {
    fn->typeParams = parseTypeParams();
  }
  bool outerGenerator = this->parser.inGenerator;
  this->parser.inGenerator = generator;
  Parser::ParamPatterns patterns;
  parseParams(fn, &patterns);

  if (this->parser.cursor().accept(Tok::Colon)) {
    fn->returnType = this->parser.types().parseReturnType();
  }

  if (this->parser.tok.is(Tok::LBrace)) {
    llvm::SaveAndRestore<bool> inGenerator(this->parser.inGenerator, generator);
    fn->body = this->parser.stmts().parseBlock();
    addPrologue(fn, patterns);
  } else if (!expression &&
             (this->parser.cursor().accept(Tok::Semi) || this->parser.tok.newlineBefore ||
              this->parser.tok.is(Tok::RBrace) || this->parser.tok.is(Tok::Eof))) {
  } else {
    this->parser.cursor().error(this->parser.tok.loc(), "expected '{' to start the function body");
    fn->body = this->parser.ctx.make<BlockStmt>(this->parser.tok.loc());
  }
  this->parser.inGenerator = outerGenerator;
  fn->source = llvm::StringRef(start, static_cast<size_t>(this->parser.prevEnd - start));

  return fn;
}

void FunctionParser::parseParams(FunctionDecl *fn, Parser::ParamPatterns *patterns) {
  this->parser.cursor().expect(Tok::LParen, fn->isArrow() ? "to start the parameters"
                                                          : "after the function name");
  while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
    Param param;
    param.decorators = this->parser.classes().parseDecorators();
    if (this->parser.recovering) {
      break;
    }
    while (this->parser.tok.is(Tok::Identifier) &&
           (this->parser.tok.text == "public" || this->parser.tok.text == "private" ||
            this->parser.tok.text == "protected" || this->parser.tok.text == "readonly" ||
            this->parser.tok.text == "override") &&
           this->parser.cursor().peek().is(Tok::Identifier)) {
      if (!fn->isConstructor()) {
        this->parser.cursor().report(this->parser.tok.loc(),
                                     "'" + this->parser.tok.text +
                                         "' before a parameter makes it a field, which only "
                                         "a constructor's parameter can be");
      }
      if (!param.property) {
        param.property = ParamProperty{};
      }
      if (this->parser.tok.text == "readonly") {
        param.property->readonly = true;
      } else if (this->parser.tok.text == "private") {
        param.property->access = Access::Private;
      } else if (this->parser.tok.text == "protected") {
        param.property->access = Access::Protected;
      }
      this->parser.cursor().advance();
    }
    bool rest = this->parser.cursor().accept(Tok::Ellipsis);
    param.loc = this->parser.tok.loc();
    if (this->parser.tok.is(Tok::Identifier)) {
      param.name = this->parser.tok.text;
      this->parser.cursor().advance();
    } else if ((this->parser.tok.is(Tok::LBrace) || this->parser.tok.is(Tok::LBracket)) && !rest) {
      const char *start = this->parser.tok.text.data();
      std::unique_ptr<Parser::Pattern> pattern = this->parser.patterns().parseBindingPattern();
      param.name = this->parser.ctx.hiddenName();
      param.pattern = this->parser.cursor().textFrom(start);
      if (patterns) {
        patterns->emplace_back(param.name, std::move(pattern));
      }
    } else {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  rest ? "expected a parameter name after '...'"
                                       : "expected a parameter name");
      break;
    }
    bool optional = this->parser.cursor().accept(Tok::Question);
    if (this->parser.cursor().accept(Tok::Colon)) {
      param.type = this->parser.types().parseType();
    }
    Expr *defaultValue = nullptr;
    if (this->parser.tok.is(Tok::Assign)) {
      if (optional) {
        this->parser.cursor().report(
            this->parser.tok.loc(),
            "a parameter with a default value is optional already; remove the '?'");
      }
      this->parser.cursor().advance();
      defaultValue = this->parser.exprs().parseExpr();
    }
    if (rest && (optional || defaultValue)) {
      this->parser.cursor().report(param.loc,
                                   "a rest parameter cannot be optional or have a default value");
    }
    if (rest) {
      param.form = RestParam{};
    } else if (defaultValue) {
      param.form = DefaultedParam{.defaultValue = defaultValue};
    } else if (optional) {
      param.form = OptionalParam{};
    }
    fn->params.push_back(param);
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
    if (rest && !this->parser.tok.is(Tok::RParen)) {
      this->parser.cursor().report(param.loc, "a rest parameter must be the last parameter");
    }
  }
  this->parser.cursor().expect(Tok::RParen, "to close the parameter list");
}

void FunctionParser::addPrologue(FunctionDecl *fn, Parser::ParamPatterns &patterns) {
  if (patterns.empty() || !fn->body) {
    return;
  }
  auto *decl = this->parser.ctx.make<VarDeclStmt>(fn->body->loc(), false);
  for (auto &[name, pattern] : patterns) {
    this->parser.patterns().declareElements(decl, *pattern, name);
  }
  fn->body->body.insert(fn->body->body.begin(), decl);
}

bool FunctionParser::asyncFunctionAhead() {
  return this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "async" &&
         this->parser.cursor().peek().is(Tok::KwFunction) &&
         !this->parser.cursor().peek().newlineBefore;
}

Expr *FunctionParser::tryAsyncFunction() {
  llvm::SMLoc loc = this->parser.tok.loc();
  if (asyncFunctionAhead()) {
    this->parser.cursor().advance();
    return this->parser.ctx.make<FunctionExpr>(loc, parseFunction(true, true));
  }
  const Token &next = this->parser.cursor().peek();
  if (next.newlineBefore ||
      !(next.is(Tok::Identifier) || next.is(Tok::LParen) || next.is(Tok::Less))) {
    return nullptr;
  }
  Parser::Snapshot start = this->parser.cursor().snapshot();
  this->parser.cursor().advance();
  if (this->parser.tok.is(Tok::Less)) {
    const char *text = this->parser.tok.text.data();
    bool muted = this->parser.diags.setMuted(true);
    this->parser.recovering = false;
    std::vector<TypeParamDecl> typeParams = parseTypeParams();
    bool arrow = !this->parser.recovering && this->parser.tok.is(Tok::LParen) && arrowAhead();
    this->parser.diags.setMuted(muted);
    if (!arrow) {
      this->parser.cursor().rewind(std::move(start));
      return nullptr;
    }
    this->parser.recovering = start.recovering;
    FunctionExpr *fe = parseArrow(std::move(typeParams), text);
    fe->fn->isAsync = true;
    return fe;
  }
  bool arrow = this->parser.tok.is(Tok::Identifier)
                   ? this->parser.cursor().peek().is(Tok::Arrow) &&
                         !this->parser.cursor().peek().newlineBefore
                   : arrowAhead();
  if (!arrow) {
    this->parser.cursor().rewind(std::move(start));
    return nullptr;
  }
  FunctionExpr *fe = parseArrow();
  fe->fn->isAsync = true;

  return fe;
}

bool FunctionParser::arrowAhead() {
  Parser::Snapshot start = this->parser.cursor().snapshot();
  bool muted = this->parser.diags.setMuted(true);
  this->parser.recovering = false;
  FunctionDecl probe(this->parser.tok.loc(), "");
  probe.syntax = ArrowFunction{};
  parseParams(&probe);
  bool arrow = !this->parser.recovering;
  if (arrow && this->parser.cursor().accept(Tok::Colon)) {
    this->parser.types().parseType();
    arrow = !this->parser.recovering;
  }
  arrow = arrow && this->parser.tok.is(Tok::Arrow);
  this->parser.diags.setMuted(muted);
  this->parser.cursor().rewind(std::move(start));

  return arrow;
}

FunctionExpr *FunctionParser::parseArrow(std::vector<TypeParamDecl> typeParams, const char *start) {
  if (!start) {
    start = this->parser.tok.text.data();
  }
  llvm::SMLoc loc = this->parser.tok.loc();
  auto *fn = this->parser.ctx.make<FunctionDecl>(loc, "");
  fn->place = FunctionPlace::EXPRESSION;
  fn->syntax = ArrowFunction{};
  fn->typeParams = std::move(typeParams);
  llvm::SaveAndRestore<bool> inGenerator(this->parser.inGenerator, false);
  Parser::ParamPatterns patterns;
  if (this->parser.tok.is(Tok::Identifier)) {
    Param param;
    param.loc = this->parser.tok.loc();
    param.name = this->parser.tok.text;
    this->parser.cursor().advance();
    fn->params.push_back(param);
  } else {
    parseParams(fn, &patterns);
    if (this->parser.cursor().accept(Tok::Colon)) {
      fn->returnType = this->parser.types().parseReturnType();
    }
  }
  if (this->parser.tok.is(Tok::Arrow) && this->parser.tok.newlineBefore) {
    this->parser.cursor().report(this->parser.tok.loc(),
                                 "'=>' must be on the same line as the parameters");
  }
  this->parser.cursor().expect(Tok::Arrow, "after the parameters of the arrow function");
  if (this->parser.tok.is(Tok::LBrace)) {
    fn->body = this->parser.stmts().parseBlock();
    if (fn->body->body.size() == 1) {
      if (auto *labeled = dyn_cast<LabeledStmt>(fn->body->body[0]);
          labeled && isa<ExprStmt>(labeled->body)) {
        this->parser.cursor().report(
            labeled->loc(), "'{' after '=>' starts the body of the function, so '" +
                                labeled->label + ":' is a label, and the function returns nothing");
      }
    }
  } else {
    llvm::SMLoc valueLoc = this->parser.tok.loc();
    Expr *value = this->parser.exprs().parseExpr();
    fn->body = this->parser.ctx.make<BlockStmt>(valueLoc);
    fn->body->body.push_back(this->parser.ctx.make<ReturnStmt>(valueLoc, value));
    fn->syntax = ArrowFunction{.concise = true};
  }
  addPrologue(fn, patterns);
  fn->source = llvm::StringRef(start, static_cast<size_t>(this->parser.prevEnd - start));

  return this->parser.ctx.make<FunctionExpr>(loc, fn);
}

std::vector<TypeParamDecl> FunctionParser::parseTypeParams() {
  std::vector<TypeParamDecl> params;
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::Greater) && !this->parser.tok.is(Tok::Eof)) {
    if ((this->parser.tok.is(Tok::KwConst) || this->parser.tok.is(Tok::KwIn) ||
         (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "out")) &&
        this->parser.cursor().peek().is(Tok::Identifier)) {
      this->parser.cursor().report(this->parser.tok.loc(),
                                   "'" + this->parser.tok.text +
                                       "' before a type parameter is not supported; remove it");
      this->parser.cursor().advance();
    }
    TypeParamDecl param;
    param.loc = this->parser.tok.loc();
    if (!this->parser.tok.is(Tok::Identifier)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected a type parameter name, found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      return params;
    }
    param.name = this->parser.tok.text;
    this->parser.cursor().advance();
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "extends") {
      this->parser.cursor().advance();
      param.constraint = this->parser.types().parseType();
    }
    if (this->parser.cursor().accept(Tok::Assign)) {
      param.defaultType = this->parser.types().parseType();
    }
    params.push_back(std::move(param));
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::Greater, "to close the type parameters");

  return params;
}

}
