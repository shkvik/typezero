#pragma once

#include "ast/Ast.h"
#include "lex/Lexer.h"

namespace tz {

// Recursive-descent parser for the supported subset of TypeScript.
//
// After a syntax error the parser skips to the next statement and suppresses
// further errors until then, so one mistake yields one message.
class Parser {
public:
  Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags);

  SourceFile parseSourceFile();

private:
  // Tokens
  void advance();
  bool accept(Tok kind);
  bool expect(Tok kind, llvm::StringRef context = {});
  void expectSemicolon();
  // A syntax error: the parser is lost until the next statement.
  void error(llvm::SMLoc loc, const llvm::Twine &message);
  // An error that leaves the parser on track, e.g. an unsupported feature.
  void report(llvm::SMLoc loc, const llvm::Twine &message);
  void synchronize();

  // Statements
  Stmt *parseStatement();
  BlockStmt *parseBlock();
  FunctionDecl *parseFunction();
  VarDeclStmt *parseVarDecl();
  Stmt *parseIf();
  Stmt *parseWhile();
  Stmt *parseDoWhile();
  Stmt *parseFor();
  Stmt *parseReturn();

  // Types
  TypeRef parseType();

  // Expressions, from lowest to highest precedence
  Expr *parseExpr();
  Expr *parseConditional();
  Expr *parseBinary(int minPrec);
  Expr *parseUnary();
  Expr *parsePostfix();
  Expr *parseCallOrMember();
  Expr *parsePrimary();
  Expr *parseIntLiteral();
  Expr *parseFloatLiteral();

  Lexer &lexer_;
  AstContext &ctx_;
  Diagnostics &diags_;
  Token tok_;
  bool recovering_ = false;
};

} // namespace tz
