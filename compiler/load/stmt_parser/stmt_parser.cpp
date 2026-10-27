#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
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

StmtParser::StmtParser(Parser &parser) : parser_(parser) {}

Stmt *StmtParser::parseStatement(bool topLevel) {
  llvm::SMLoc loc = parser_.tok_.loc();
  switch (parser_.tok_.kind) {
  case Tok::At: {
    std::vector<Decorator> decorators = parser_.classes().parseDecorators();
    Stmt *stmt = parseStatement(topLevel);
    if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
    } else {
      parser_.classes().rejectDecorators(decorators);
    }
    return stmt;
  }
  case Tok::LBrace:
    return parseBlock();
  case Tok::KwFunction:
    return parser_.functions().parseFunctionDecl();
  case Tok::KwVar:
    parser_.cursor().report(loc, "'var' is not supported; use 'let' or 'const'");
    [[fallthrough]];
  case Tok::KwLet:
  case Tok::KwConst: {
    if (parser_.tok_.is(Tok::KwConst) && parser_.cursor().peek().is(Tok::KwEnum)) {
      parser_.cursor().advance();
      return parser_.decls().parseEnum();
    }
    VarDeclStmt *decl = parser_.vars().parseVarDecl();
    parser_.cursor().expectSemicolon();
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
    return parser_.decls().parseEnum();
  case Tok::KwReturn:
    return parseReturn();
  case Tok::KwBreak:
  case Tok::KwContinue:
    return parseBreakOrContinue();
  case Tok::KwDebugger:
    parser_.cursor().advance();
    parser_.cursor().expectSemicolon();
    return parser_.ctx_.make<DebuggerStmt>(loc);
  case Tok::Semi:
    parser_.cursor().advance();
    return parser_.ctx_.make<EmptyStmt>(loc);
  case Tok::Identifier:
    if (parser_.tok_.text == "import" && !parser_.cursor().peek().is(Tok::LParen) &&
        !parser_.cursor().peek().is(Tok::Dot)) {
      return parser_.modules().parseImport(topLevel);
    }
    if (parser_.tok_.text == "export") {
      return parser_.modules().parseExport(topLevel);
    }
    if ((parser_.tok_.text == "namespace" || parser_.tok_.text == "module" ||
         parser_.tok_.text == "declare") &&
        (parser_.cursor().peek().is(Tok::Identifier) || isKeyword(parser_.cursor().peek().kind) ||
         parser_.cursor().peek().is(Tok::StringLiteral)) &&
        !parser_.cursor().peek().newlineBefore) {
      if (parser_.tok_.text != "declare") {
        return skipDeclaration(loc, "'" + parser_.tok_.text +
                                        "' is not supported: it is TypeScript's old way to group "
                                        "code; put the code in a file of its own and import it "
                                        "by name");
      }
      llvm::StringRef what = parser_.cursor().peek().text;
      if (what == "module" || what == "global" || what == "namespace") {
        return skipDeclaration(loc, "'declare " + what +
                                        "' is not supported: every file is a module of the "
                                        "program; import what it needs by name");
      }
      return skipDeclaration(loc, "'declare' is not supported: the compiler needs the code of "
                                  "every declaration; write the declaration itself, or import it");
    }
    if (parser_.cursor().peek().is(Tok::Colon)) {
      llvm::StringRef label = parser_.tok_.text;
      parser_.cursor().advance();
      parser_.cursor().advance();
      return parser_.ctx_.make<LabeledStmt>(loc, label, parseStatement());
    }
    if (parser_.tok_.text == "type" && parser_.cursor().peek().is(Tok::Identifier) &&
        !parser_.cursor().peek().newlineBefore) {
      return parser_.decls().parseTypeAlias();
    }
    if (parser_.tok_.text == "interface" && parser_.cursor().peek().is(Tok::Identifier) &&
        !parser_.cursor().peek().newlineBefore) {
      return parser_.decls().parseInterface();
    }
    if (parser_.tok_.text == "class" && parser_.cursor().peek().is(Tok::Identifier) &&
        !parser_.cursor().peek().newlineBefore) {
      return parser_.classes().parseClass();
    }
    if (parser_.tok_.text == "throw") {
      return parseThrow();
    }
    if (parser_.tok_.text == "try" && parser_.cursor().peek().is(Tok::LBrace)) {
      return parseTry();
    }
    if ((parser_.tok_.text == "catch" || parser_.tok_.text == "finally") &&
        (parser_.cursor().peek().is(Tok::LBrace) || parser_.cursor().peek().is(Tok::LParen))) {
      parser_.cursor().report(loc, "there is no '" + parser_.tok_.text +
                                       "' in tz: an error is a value; a function that can fail "
                                       "gives a 'Result', checked with 'if'");
      parser_.cursor().advance();
      if (parser_.cursor().accept(Tok::LParen)) {
        while (!parser_.tok_.is(Tok::Eof) && !parser_.cursor().accept(Tok::RParen)) {
          parser_.cursor().advance();
        }
      }
      parseBlock();
      return parser_.ctx_.make<EmptyStmt>(loc);
    }
    if (parser_.tok_.text == "abstract" && parser_.cursor().peek().is(Tok::Identifier) &&
        parser_.cursor().peek().text == "class" && !parser_.cursor().peek().newlineBefore) {
      parser_.cursor().advance();
      ClassDecl *cls = parser_.classes().parseClass();
      cls->isAbstract = true;
      return cls;
    }
    if (parser_.functions().asyncFunctionAhead()) {
      parser_.cursor().advance();
      return parser_.functions().parseFunctionDecl(true);
    }
    [[fallthrough]];
  default: {
    Expr *expr = parser_.exprs().parseSequence();
    if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
      if (isa<ArrayLitExpr>(assign->target) || isa<ObjectLitExpr>(assign->target)) {
        Stmt *stmt = parser_.patterns().assignPattern(
            loc, *parser_.patterns().patternFromLiteral(assign->target), assign->value);
        parser_.cursor().expectSemicolon();
        return stmt;
      }
    }
    parser_.cursor().expectSemicolon();
    return parser_.ctx_.make<ExprStmt>(loc, expr);
  }
  }
}

Stmt *StmtParser::skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message) {
  parser_.cursor().report(loc, message);
  parser_.cursor().advance();
  while (!parser_.tok_.is(Tok::Eof) && !parser_.tok_.is(Tok::RBrace)) {
    if (parser_.tok_.is(Tok::LBrace)) {
      parser_.cursor().advance();
      parser_.cursor().skipToClosingBrace();
      break;
    }
    if (parser_.cursor().accept(Tok::Semi)) {
      break;
    }
    parser_.cursor().advance();
    if (parser_.tok_.newlineBefore) {
      break;
    }
  }

  return parser_.ctx_.make<EmptyStmt>(loc);
}

Stmt *StmtParser::parseBreakOrContinue() {
  llvm::SMLoc loc = parser_.tok_.loc();
  bool isBreak = parser_.tok_.is(Tok::KwBreak);
  parser_.cursor().advance();
  llvm::StringRef label;
  if (parser_.tok_.is(Tok::Identifier) && !parser_.tok_.newlineBefore) {
    label = parser_.tok_.text;
    parser_.cursor().advance();
  }
  parser_.cursor().expectSemicolon();
  if (isBreak) {
    return parser_.ctx_.make<BreakStmt>(loc, label);
  }

  return parser_.ctx_.make<ContinueStmt>(loc, label);
}

Stmt *StmtParser::parseSwitch() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  parser_.cursor().expect(Tok::LParen, "after 'switch'");
  Expr *discriminant = parser_.exprs().parseSequence();
  parser_.cursor().expect(Tok::RParen, "after the value of 'switch'");
  auto *stmt = parser_.ctx_.make<SwitchStmt>(loc, discriminant);
  parser_.cursor().expect(Tok::LBrace, "to start the cases of 'switch'");
  bool hasDefault = false;
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    SwitchCase c;
    c.loc = parser_.tok_.loc();
    if (parser_.cursor().accept(Tok::KwCase)) {
      c.test = parser_.exprs().parseExpr();
    } else if (parser_.tok_.is(Tok::KwDefault)) {
      if (hasDefault) {
        parser_.cursor().report(parser_.tok_.loc(), "a 'switch' can have only one 'default'");
      }
      hasDefault = true;
      parser_.cursor().advance();
    } else {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected 'case' or 'default', found ") +
                                 tokSpelling(parser_.tok_.kind));
      break;
    }
    parser_.cursor().expect(Tok::Colon, c.test ? "after the value of 'case'" : "after 'default'");
    while (!parser_.tok_.is(Tok::KwCase) && !parser_.tok_.is(Tok::KwDefault) &&
           !parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
      c.body.push_back(parseStatement());
      if (parser_.recovering_) {
        parser_.cursor().synchronize();
      }
    }
    stmt->cases.push_back(std::move(c));
  }
  parser_.cursor().expect(Tok::RBrace, "to close 'switch'");

  return stmt;
}

BlockStmt *StmtParser::parseBlock() {
  auto *block = parser_.ctx_.make<BlockStmt>(parser_.tok_.loc());
  parser_.cursor().expect(Tok::LBrace);
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    block->body.push_back(parseStatement());
    if (parser_.recovering_) {
      parser_.cursor().synchronize();
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the block");

  return block;
}

Stmt *StmtParser::parseIf() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  parser_.cursor().expect(Tok::LParen, "after 'if'");
  Expr *cond = parser_.exprs().parseSequence();
  parser_.cursor().expect(Tok::RParen, "after the condition");
  Stmt *thenStmt = parseStatement();
  Stmt *elseStmt = nullptr;
  if (parser_.cursor().accept(Tok::KwElse)) {
    elseStmt = parseStatement();
  }

  return parser_.ctx_.make<IfStmt>(loc, cond, thenStmt, elseStmt);
}

Stmt *StmtParser::parseWhile() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  parser_.cursor().expect(Tok::LParen, "after 'while'");
  Expr *cond = parser_.exprs().parseSequence();
  parser_.cursor().expect(Tok::RParen, "after the condition");
  Stmt *body = parseStatement();

  return parser_.ctx_.make<WhileStmt>(loc, cond, body);
}

Stmt *StmtParser::parseDoWhile() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  Stmt *body = parseStatement();
  parser_.cursor().expect(Tok::KwWhile, "after the body of 'do'");
  parser_.cursor().expect(Tok::LParen, "after 'while'");
  Expr *cond = parser_.exprs().parseSequence();
  parser_.cursor().expect(Tok::RParen, "after the condition");
  parser_.cursor().accept(Tok::Semi);

  return parser_.ctx_.make<DoWhileStmt>(loc, body, cond);
}

Stmt *StmtParser::parseFor() {
  llvm::SMLoc forLoc = parser_.tok_.loc();
  auto *loop = parser_.ctx_.make<ForStmt>(forLoc);
  parser_.cursor().advance();
  llvm::SMLoc awaitLoc;
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "await") {
    awaitLoc = parser_.tok_.loc();
    parser_.cursor().advance();
  }
  parser_.cursor().expect(Tok::LParen, "after 'for'");

  llvm::SaveAndRestore<bool> noIn(parser_.noIn_, true);
  if (parser_.tok_.is(Tok::KwLet) || parser_.tok_.is(Tok::KwConst) || parser_.tok_.is(Tok::KwVar)) {
    if (parser_.tok_.is(Tok::KwVar)) {
      parser_.cursor().error(parser_.tok_.loc(), "'var' is not supported; use 'let' or 'const'");
    }
    auto *decl = parser_.ctx_.make<VarDeclStmt>(parser_.tok_.loc(), parser_.tok_.is(Tok::KwConst));
    parser_.cursor().advance();
    VarDeclarator first;
    std::unique_ptr<Parser::Pattern> pattern;
    if (parser_.tok_.is(Tok::LBracket) || parser_.tok_.is(Tok::LBrace)) {
      pattern = parser_.patterns().parseBindingPattern();
    } else if (!parser_.vars().parseBinding(first)) {
      return loop;
    }
    bool forIn = parser_.tok_.is(Tok::KwIn);
    if ((parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "of") || forIn) {
      if (forIn && pattern) {
        parser_.cursor().report(
            pattern->loc,
            "the variable of 'for...in' is a key, a string, and cannot be destructured");
      }
      parser_.cursor().advance();
      parser_.noIn_ = false;
      auto *forOf = parser_.ctx_.make<ForOfStmt>(forLoc);
      forOf->isConst = decl->isConst;
      forOf->keys = forIn;
      forOf->isAwait = awaitLoc.isValid();
      if (forIn && forOf->isAwait) {
        parser_.cursor().report(
            awaitLoc, "'for await' goes over the values of an iterable, with 'of': 'for await "
                      "(const x of xs)'");
      }
      forOf->iterable = parser_.exprs().parseExpr();
      parser_.cursor().expect(Tok::RParen, "to close the loop header");
      forOf->body = parseStatement();
      if (!pattern || forIn) {
        forOf->binding = first;
        if (pattern) {
          forOf->binding.name = parser_.ctx_.hiddenName();
          forOf->binding.loc = pattern->loc;
        }
        return forOf;
      }
      forOf->binding.name = parser_.ctx_.hiddenName();
      forOf->binding.loc = pattern->loc;
      forOf->binding.destructured = true;
      forOf->binding.objectPattern = pattern->object;
      auto *elements = parser_.ctx_.make<VarDeclStmt>(pattern->loc, decl->isConst);
      parser_.patterns().declareElements(elements, *pattern, forOf->binding.name);
      auto *body = parser_.ctx_.make<BlockStmt>(forOf->body->loc());
      body->body = {elements, forOf->body};
      forOf->body = body;
      return forOf;
    }
    if (pattern) {
      if (!parser_.cursor().expect(Tok::Assign, "after the destructuring pattern")) {
        return loop;
      }
      parser_.patterns().declarePattern(decl, *pattern, parser_.exprs().parseExpr(), std::nullopt);
    } else {
      if (parser_.cursor().accept(Tok::Assign)) {
        first.init = parser_.exprs().parseExpr();
      }
      decl->decls.push_back(first);
    }
    if (parser_.cursor().accept(Tok::Comma)) {
      parser_.vars().parseDeclarators(decl);
    }
    loop->init = decl;
  } else if (!parser_.tok_.is(Tok::Semi)) {
    llvm::SMLoc loc = parser_.tok_.loc();
    loop->init = parser_.ctx_.make<ExprStmt>(loc, parser_.exprs().parseSequence());
  }
  if (parser_.tok_.is(Tok::KwIn) ||
      (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "of")) {
    parser_.cursor().error(parser_.tok_.loc(),
                           parser_.tok_.is(Tok::KwIn)
                               ? "'for...in' needs a new variable: 'for (const k in ...)'"
                               : "'for...of' needs a new variable: 'for (const x of ...)'");
    return loop;
  }
  parser_.noIn_ = false;
  if (awaitLoc.isValid()) {
    parser_.cursor().report(
        awaitLoc, "'for await' goes over the values of an iterable: 'for await (const x of "
                  "xs)'");
  }
  parser_.cursor().expect(Tok::Semi, "after the loop initializer");

  if (!parser_.tok_.is(Tok::Semi)) {
    loop->cond = parser_.exprs().parseSequence();
  }
  parser_.cursor().expect(Tok::Semi, "after the loop condition");

  if (!parser_.tok_.is(Tok::RParen)) {
    loop->step = parser_.exprs().parseSequence();
  }
  parser_.cursor().expect(Tok::RParen, "to close the loop header");

  loop->body = parseStatement();

  return loop;
}

Stmt *StmtParser::parseReturn() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  Expr *value = nullptr;
  if (!parser_.tok_.is(Tok::Semi) && !parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof) &&
      !parser_.tok_.newlineBefore) {
    value = parser_.exprs().parseSequence();
  }
  parser_.cursor().expectSemicolon();

  return parser_.ctx_.make<ReturnStmt>(loc, value);
}

Stmt *StmtParser::parseThrow() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().report(loc, "there is no 'throw' in tz: an error is a value; return it, "
                               "'return err(...)', and give the function a 'Result'");
  parser_.cursor().advance();
  if (!parser_.tok_.newlineBefore && !parser_.tok_.is(Tok::Semi) && !parser_.tok_.is(Tok::RBrace) &&
      !parser_.tok_.is(Tok::Eof)) {
    parser_.exprs().parseSequence();
    parser_.cursor().expectSemicolon();
  }

  return parser_.ctx_.make<EmptyStmt>(loc);
}

Stmt *StmtParser::parseTry() {
  parser_.cursor().report(parser_.tok_.loc(),
                          "there is no 'try' in tz: an error is a value; a function that can "
                          "fail gives a 'Result', checked with 'if'");
  parser_.cursor().advance();
  BlockStmt *block = parseBlock();
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "catch") {
    parser_.cursor().advance();
    if (parser_.cursor().accept(Tok::LParen)) {
      while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
        parser_.cursor().advance();
      }
      parser_.cursor().expect(Tok::RParen, "after the variable of 'catch'");
    }
    parseBlock();
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "finally") {
    parser_.cursor().advance();
    parseBlock();
  }

  return block;
}

}
