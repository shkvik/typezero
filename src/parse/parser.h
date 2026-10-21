#pragma once

#include "ast/ast.h"
#include "lex/lexer.h"

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
  // The token after the current one.
  const Token &peek();
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
  void parseDeclarators(VarDeclStmt *decl);
  bool parseBinding(VarDeclarator &d);
  Stmt *parseIf();
  Stmt *parseWhile();
  Stmt *parseDoWhile();
  Stmt *parseFor();
  Stmt *parseSwitch();
  Stmt *parseReturn();
  Stmt *parseBreakOrContinue();
  Stmt *parseTypeAlias();
  Stmt *parseEnum();

  // Types
  TypeRef parseType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  std::vector<TypeRef> parseTypeArgs();

  // Expressions, from lowest to highest precedence
  Expr *parseExpr();
  Expr *parseConditional();
  Expr *parseBinary(int minPrec);
  Expr *parseUnary();
  Expr *parsePostfix();
  Expr *parseCallOrMember();
  Expr *parsePrimary();
  Expr *parseTemplate();
  Expr *parseArrayLiteral();
  Expr *parseNew();
  Expr *parseIntLiteral();
  Expr *parseFloatLiteral();

  Lexer &lexer_;
  AstContext &ctx_;
  Diagnostics &diags_;
  Token tok_;
  Token peeked_;
  bool hasPeeked_ = false;
  bool recovering_ = false;
};

} // namespace tz
