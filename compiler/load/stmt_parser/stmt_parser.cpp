#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
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

StmtParser::StmtParser(Parser &parser)
    : parser(parser) {}

Stmt *StmtParser::parseStatement(bool topLevel) {
  llvm::SMLoc loc = this->parser.tok.loc();
  switch (this->parser.tok.kind) {
    case Tok::At: {
      std::vector<Decorator> decorators = this->parser.classes().parseDecorators();
      Stmt *stmt = parseStatement(topLevel);
      if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
        cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
      } else {
        this->parser.classes().rejectDecorators(decorators);
      }
      return stmt;
    }
    case Tok::LBrace:
      return parseBlock();
    case Tok::KwFunction:
      return this->parser.functions().parseFunctionDecl();
    case Tok::KwVar:
      this->parser.cursor().report(loc, "'var' is not supported; use 'let' or 'const'");
      [[fallthrough]];
    case Tok::KwLet:
    case Tok::KwConst: {
      if (this->parser.tok.is(Tok::KwConst) && this->parser.cursor().peek().is(Tok::KwEnum)) {
        this->parser.cursor().advance();
        return this->parser.decls().parseEnum();
      }
      VarDeclStmt *decl = this->parser.vars().parseVarDecl();
      this->parser.cursor().expectSemicolon();
      return decl;
    }
    case Tok::KwIf:
      return parseIf();
    case Tok::KwWhile:
      return parseWhile();
    case Tok::KwDo:
      return parseDoWhile();
    case Tok::KwFor:
      return parseFor();
    case Tok::KwSwitch:
      return parseSwitch();
    case Tok::KwEnum:
      return this->parser.decls().parseEnum();
    case Tok::KwReturn:
      return parseReturn();
    case Tok::KwBreak:
    case Tok::KwContinue:
      return parseBreakOrContinue();
    case Tok::KwDebugger:
      this->parser.cursor().advance();
      this->parser.cursor().expectSemicolon();
      return this->parser.ctx.make<DebuggerStmt>(loc);
    case Tok::Semi:
      this->parser.cursor().advance();
      return this->parser.ctx.make<EmptyStmt>(loc);
    case Tok::Identifier:
      if (this->parser.tok.text == "import" && !this->parser.cursor().peek().is(Tok::LParen) &&
          !this->parser.cursor().peek().is(Tok::Dot)) {
        return this->parser.modules().parseImport(topLevel);
      }
      if (this->parser.tok.text == "export") {
        return this->parser.modules().parseExport(topLevel);
      }
      if ((this->parser.tok.text == "namespace" || this->parser.tok.text == "module" ||
           this->parser.tok.text == "declare") &&
          (this->parser.cursor().peek().is(Tok::Identifier) ||
           TokenKinds::isKeyword(this->parser.cursor().peek().kind) ||
           this->parser.cursor().peek().is(Tok::StringLiteral)) &&
          !this->parser.cursor().peek().newlineBefore) {
        if (this->parser.tok.text != "declare") {
          return skipDeclaration(loc, "'" + this->parser.tok.text +
                                          "' is not supported: it is TypeScript's old way to group "
                                          "code; put the code in a file of its own and import it "
                                          "by name");
        }
        if (this->parser.library_ && this->parser.cursor().peek().is(Tok::KwFunction)) {
          this->parser.cursor().advance();
          return this->parser.functions().parseDeclaredFunction();
        }
        llvm::StringRef what = this->parser.cursor().peek().text;
        if (what == "module" || what == "global" || what == "namespace") {
          return skipDeclaration(loc, "'declare " + what +
                                          "' is not supported: every file is a module of the "
                                          "program; import what it needs by name");
        }
        return skipDeclaration(loc,
                               "'declare' is not supported: the compiler needs the code of "
                               "every declaration; write the declaration itself, or import it");
      }
      if (this->parser.cursor().peek().is(Tok::Colon)) {
        llvm::StringRef label = this->parser.tok.text;
        this->parser.cursor().advance();
        this->parser.cursor().advance();
        return this->parser.ctx.make<LabeledStmt>(loc, label, parseStatement());
      }
      if (this->parser.tok.text == "type" && this->parser.cursor().peek().is(Tok::Identifier) &&
          !this->parser.cursor().peek().newlineBefore) {
        return this->parser.decls().parseTypeAlias();
      }
      if (this->parser.tok.text == "interface" &&
          this->parser.cursor().peek().is(Tok::Identifier) &&
          !this->parser.cursor().peek().newlineBefore) {
        return this->parser.decls().parseInterface();
      }
      if (this->parser.tok.text == "class" && this->parser.cursor().peek().is(Tok::Identifier) &&
          !this->parser.cursor().peek().newlineBefore) {
        return this->parser.classes().parseClass();
      }
      if (this->parser.tok.text == "throw") {
        return parseThrow();
      }
      if (this->parser.tok.text == "try" && this->parser.cursor().peek().is(Tok::LBrace)) {
        return parseTry();
      }
      if ((this->parser.tok.text == "catch" || this->parser.tok.text == "finally") &&
          (this->parser.cursor().peek().is(Tok::LBrace) ||
           this->parser.cursor().peek().is(Tok::LParen))) {
        this->parser.cursor().report(loc,
                                     "there is no '" + this->parser.tok.text +
                                         "' in tz: an error is a value; a function that can fail "
                                         "gives a 'Result', checked with 'if'");
        this->parser.cursor().advance();
        if (this->parser.cursor().accept(Tok::LParen)) {
          while (!this->parser.tok.is(Tok::Eof) && !this->parser.cursor().accept(Tok::RParen)) {
            this->parser.cursor().advance();
          }
        }
        parseBlock();
        return this->parser.ctx.make<EmptyStmt>(loc);
      }
      if (this->parser.tok.text == "abstract" && this->parser.cursor().peek().is(Tok::Identifier) &&
          this->parser.cursor().peek().text == "class" &&
          !this->parser.cursor().peek().newlineBefore) {
        this->parser.cursor().advance();
        ClassDecl *cls = this->parser.classes().parseClass();
        cls->isAbstract = true;
        return cls;
      }
      if (this->parser.functions().asyncFunctionAhead()) {
        this->parser.cursor().advance();
        return this->parser.functions().parseFunctionDecl(true);
      }
      [[fallthrough]];
    default: {
      Expr *expr = this->parser.exprs().parseSequence();
      if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
        if (isa<ArrayLitExpr>(assign->target) || isa<ObjectLitExpr>(assign->target)) {
          Stmt *stmt = this->parser.patterns().assignPattern(
              loc, *this->parser.patterns().patternFromLiteral(assign->target), assign->value);
          this->parser.cursor().expectSemicolon();
          return stmt;
        }
      }
      this->parser.cursor().expectSemicolon();
      return this->parser.ctx.make<ExprStmt>(loc, expr);
    }
  }
}

Stmt *StmtParser::skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message) {
  this->parser.cursor().report(loc, message);
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::Eof) && !this->parser.tok.is(Tok::RBrace)) {
    if (this->parser.tok.is(Tok::LBrace)) {
      this->parser.cursor().advance();
      this->parser.cursor().skipToClosingBrace();
      break;
    }
    if (this->parser.cursor().accept(Tok::Semi)) {
      break;
    }
    this->parser.cursor().advance();
    if (this->parser.tok.newlineBefore) {
      break;
    }
  }

  return this->parser.ctx.make<EmptyStmt>(loc);
}

Stmt *StmtParser::parseBreakOrContinue() {
  llvm::SMLoc loc = this->parser.tok.loc();
  bool isBreak = this->parser.tok.is(Tok::KwBreak);
  this->parser.cursor().advance();
  llvm::StringRef label;
  if (this->parser.tok.is(Tok::Identifier) && !this->parser.tok.newlineBefore) {
    label = this->parser.tok.text;
    this->parser.cursor().advance();
  }
  this->parser.cursor().expectSemicolon();
  if (isBreak) {
    return this->parser.ctx.make<BreakStmt>(loc, label);
  }

  return this->parser.ctx.make<ContinueStmt>(loc, label);
}

Stmt *StmtParser::parseSwitch() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  this->parser.cursor().expect(Tok::LParen, "after 'switch'");
  Expr *discriminant = this->parser.exprs().parseSequence();
  this->parser.cursor().expect(Tok::RParen, "after the value of 'switch'");
  auto *stmt = this->parser.ctx.make<SwitchStmt>(loc, discriminant);
  this->parser.cursor().expect(Tok::LBrace, "to start the cases of 'switch'");
  bool hasDefault = false;
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    SwitchCase c;
    c.loc = this->parser.tok.loc();
    if (this->parser.cursor().accept(Tok::KwCase)) {
      c.test = this->parser.exprs().parseExpr();
    } else if (this->parser.tok.is(Tok::KwDefault)) {
      if (hasDefault) {
        this->parser.cursor().report(this->parser.tok.loc(),
                                     "a 'switch' can have only one 'default'");
      }
      hasDefault = true;
      this->parser.cursor().advance();
    } else {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected 'case' or 'default', found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      break;
    }
    this->parser.cursor().expect(Tok::Colon,
                                 c.test ? "after the value of 'case'" : "after 'default'");
    while (!this->parser.tok.is(Tok::KwCase) && !this->parser.tok.is(Tok::KwDefault) &&
           !this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
      c.body.push_back(parseStatement());
      if (this->parser.recovering) {
        this->parser.cursor().synchronize();
      }
    }
    stmt->cases.push_back(std::move(c));
  }
  this->parser.cursor().expect(Tok::RBrace, "to close 'switch'");

  return stmt;
}

BlockStmt *StmtParser::parseBlock() {
  auto *block = this->parser.ctx.make<BlockStmt>(this->parser.tok.loc());
  this->parser.cursor().expect(Tok::LBrace);
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    block->body.push_back(parseStatement());
    if (this->parser.recovering) {
      this->parser.cursor().synchronize();
    }
  }
  block->end = this->parser.tok.loc();
  this->parser.cursor().expect(Tok::RBrace, "to close the block");

  return block;
}

Stmt *StmtParser::parseIf() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  this->parser.cursor().expect(Tok::LParen, "after 'if'");
  Expr *cond = this->parser.exprs().parseSequence();
  this->parser.cursor().expect(Tok::RParen, "after the condition");
  Stmt *thenStmt = parseStatement();
  Stmt *elseStmt = nullptr;
  if (this->parser.cursor().accept(Tok::KwElse)) {
    elseStmt = parseStatement();
  }

  return this->parser.ctx.make<IfStmt>(loc, cond, thenStmt, elseStmt);
}

Stmt *StmtParser::parseWhile() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  this->parser.cursor().expect(Tok::LParen, "after 'while'");
  Expr *cond = this->parser.exprs().parseSequence();
  this->parser.cursor().expect(Tok::RParen, "after the condition");
  Stmt *body = parseStatement();

  return this->parser.ctx.make<WhileStmt>(loc, cond, body);
}

Stmt *StmtParser::parseDoWhile() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  Stmt *body = parseStatement();
  this->parser.cursor().expect(Tok::KwWhile, "after the body of 'do'");
  this->parser.cursor().expect(Tok::LParen, "after 'while'");
  Expr *cond = this->parser.exprs().parseSequence();
  this->parser.cursor().expect(Tok::RParen, "after the condition");
  this->parser.cursor().accept(Tok::Semi);

  return this->parser.ctx.make<DoWhileStmt>(loc, body, cond);
}

Stmt *StmtParser::parseFor() {
  llvm::SMLoc forLoc = this->parser.tok.loc();
  auto *loop = this->parser.ctx.make<ForStmt>(forLoc);
  this->parser.cursor().advance();
  llvm::SMLoc awaitLoc;
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "await") {
    awaitLoc = this->parser.tok.loc();
    this->parser.cursor().advance();
  }
  this->parser.cursor().expect(Tok::LParen, "after 'for'");

  llvm::SaveAndRestore<bool> noIn(this->parser.noIn, true);
  if (this->parser.tok.is(Tok::KwLet) || this->parser.tok.is(Tok::KwConst) ||
      this->parser.tok.is(Tok::KwVar)) {
    if (this->parser.tok.is(Tok::KwVar)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  "'var' is not supported; use 'let' or 'const'");
    }
    auto *decl = this->parser.ctx.make<VarDeclStmt>(this->parser.tok.loc(),
                                                    this->parser.tok.is(Tok::KwConst));
    this->parser.cursor().advance();
    VarDeclarator first;
    std::unique_ptr<Parser::Pattern> pattern;
    if (this->parser.tok.is(Tok::LBracket) || this->parser.tok.is(Tok::LBrace)) {
      pattern = this->parser.patterns().parseBindingPattern();
    } else if (!this->parser.vars().parseBinding(first)) {
      return loop;
    }
    bool forIn = this->parser.tok.is(Tok::KwIn);
    if ((this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "of") || forIn) {
      if (forIn && pattern) {
        this->parser.cursor().report(
            pattern->loc,
            "the variable of 'for...in' is a key, a string, and cannot be destructured");
      }
      this->parser.cursor().advance();
      this->parser.noIn = false;
      auto *forOf = this->parser.ctx.make<ForOfStmt>(forLoc);
      forOf->isConst = decl->isConst;
      forOf->keys = forIn;
      forOf->isAwait = awaitLoc.isValid();
      if (forIn && forOf->isAwait) {
        this->parser.cursor().report(
            awaitLoc, "'for await' goes over the values of an iterable, with 'of': 'for await "
                      "(const x of xs)'");
      }
      forOf->iterable = this->parser.exprs().parseExpr();
      this->parser.cursor().expect(Tok::RParen, "to close the loop header");
      forOf->body = parseStatement();
      if (!pattern || forIn) {
        forOf->binding = first;
        if (pattern) {
          forOf->binding.name = this->parser.ctx.hiddenName();
          forOf->binding.loc = pattern->loc;
        }
        return forOf;
      }
      forOf->binding.name = this->parser.ctx.hiddenName();
      forOf->binding.loc = pattern->loc;
      if (pattern->object) {
        forOf->binding.form = ObjectPattern{};
      } else {
        forOf->binding.form = ArrayPattern{};
      }
      auto *elements = this->parser.ctx.make<VarDeclStmt>(pattern->loc, decl->isConst);
      this->parser.patterns().declareElements(elements, *pattern, forOf->binding.name);
      auto *body = this->parser.ctx.make<BlockStmt>(forOf->body->loc());
      body->body = {elements, forOf->body};
      forOf->body = body;
      return forOf;
    }
    if (pattern) {
      if (!this->parser.cursor().expect(Tok::Assign, "after the destructuring pattern")) {
        return loop;
      }
      this->parser.patterns().declarePattern(decl, *pattern, this->parser.exprs().parseExpr(),
                                             std::nullopt);
    } else {
      if (this->parser.cursor().accept(Tok::Assign)) {
        first.init = this->parser.exprs().parseExpr();
      }
      decl->decls.push_back(first);
    }
    if (this->parser.cursor().accept(Tok::Comma)) {
      this->parser.vars().parseDeclarators(decl);
    }
    loop->init = decl;
  } else if (!this->parser.tok.is(Tok::Semi)) {
    llvm::SMLoc loc = this->parser.tok.loc();
    loop->init = this->parser.ctx.make<ExprStmt>(loc, this->parser.exprs().parseSequence());
  }
  if (this->parser.tok.is(Tok::KwIn) ||
      (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "of")) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                this->parser.tok.is(Tok::KwIn)
                                    ? "'for...in' needs a new variable: 'for (const k in ...)'"
                                    : "'for...of' needs a new variable: 'for (const x of ...)'");
    return loop;
  }
  this->parser.noIn = false;
  if (awaitLoc.isValid()) {
    this->parser.cursor().report(
        awaitLoc, "'for await' goes over the values of an iterable: 'for await (const x of "
                  "xs)'");
  }
  this->parser.cursor().expect(Tok::Semi, "after the loop initializer");

  if (!this->parser.tok.is(Tok::Semi)) {
    loop->cond = this->parser.exprs().parseSequence();
  }
  this->parser.cursor().expect(Tok::Semi, "after the loop condition");

  if (!this->parser.tok.is(Tok::RParen)) {
    loop->step = this->parser.exprs().parseSequence();
  }
  this->parser.cursor().expect(Tok::RParen, "to close the loop header");

  loop->body = parseStatement();

  return loop;
}

Stmt *StmtParser::parseReturn() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  Expr *value = nullptr;
  if (!this->parser.tok.is(Tok::Semi) && !this->parser.tok.is(Tok::RBrace) &&
      !this->parser.tok.is(Tok::Eof) && !this->parser.tok.newlineBefore) {
    value = this->parser.exprs().parseSequence();
  }
  this->parser.cursor().expectSemicolon();

  return this->parser.ctx.make<ReturnStmt>(loc, value);
}

Stmt *StmtParser::parseThrow() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().report(loc, "there is no 'throw' in tz: an error is a value; return it, "
                                    "'return err(...)', and give the function a 'Result'");
  this->parser.cursor().advance();
  if (!this->parser.tok.newlineBefore && !this->parser.tok.is(Tok::Semi) &&
      !this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    this->parser.exprs().parseSequence();
    this->parser.cursor().expectSemicolon();
  }

  return this->parser.ctx.make<EmptyStmt>(loc);
}

Stmt *StmtParser::parseTry() {
  this->parser.cursor().report(this->parser.tok.loc(),
                               "there is no 'try' in tz: an error is a value; a function that can "
                               "fail gives a 'Result', checked with 'if'");
  this->parser.cursor().advance();
  BlockStmt *block = parseBlock();
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "catch") {
    this->parser.cursor().advance();
    if (this->parser.cursor().accept(Tok::LParen)) {
      while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
        this->parser.cursor().advance();
      }
      this->parser.cursor().expect(Tok::RParen, "after the variable of 'catch'");
    }
    parseBlock();
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "finally") {
    this->parser.cursor().advance();
    parseBlock();
  }

  return block;
}

}
