#pragma once

#include "ast/ast.h"
#include "lex/lexer.h"

namespace tz {

// Recursive-descent parser for the supported subset of TypeScript.
//
// After a syntax error the parser skips to the next statement and suppresses
// further errors until then, so one mistake yields one message.
//
// Destructuring never reaches the AST: the parser expands it into plain
// declarations and assignments through hidden variables (see declarePattern
// and assignPattern).
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

  // Statements. `export` is accepted, and ignored, only at the top level.
  Stmt *parseStatement(bool topLevel = false);
  BlockStmt *parseBlock();
  Stmt *parseFunctionDecl();
  // A function, or an overload signature (without a body) for tsc.
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

  // Destructuring: `[a, , b = 1, [c], ...rest]`.
  struct Pattern {
    struct Element {
      llvm::SMLoc loc;
      bool hole = false;
      bool rest = false;                // `...rest`, the last element
      llvm::StringRef name;             // a declaration's variable
      Expr *target = nullptr;           // what an assignment assigns to
      std::unique_ptr<Pattern> nested;  // or a nested pattern
      Expr *defaultValue = nullptr;     // `= value`, taken for undefined
    };
    llvm::SMLoc loc;
    std::vector<Element> elements;

    bool declares(llvm::StringRef name) const;
  };
  std::unique_ptr<Pattern> parseBindingPattern();
  // The pattern an array literal on the left of `=` stands for.
  std::unique_ptr<Pattern> patternFromLiteral(ArrayLitExpr *lit);
  // Declares the variables of `pattern`, taking their values from `value`,
  // which a hidden variable holds.
  void declarePattern(VarDeclStmt *decl, const Pattern &pattern, Expr *value,
                      std::optional<TypeRef> typeAnn);
  // The same from the array in the variable `base`.
  void declareElements(VarDeclStmt *decl, const Pattern &pattern, llvm::StringRef base);
  // `[a, b] = value` as a statement.
  Stmt *assignPattern(llvm::SMLoc loc, const Pattern &pattern, Expr *value);
  // The value of element `i` of the array in `base`: `base[i]`, with the
  // default value, or `base.slice(i)` for the rest.
  Expr *elementOf(llvm::StringRef base, size_t i, const Pattern::Element &e);
  VarDeclarator hiddenDeclarator(llvm::SMLoc loc, Expr *init);

  // Types
  TypeRef parseType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  std::vector<TypeRef> parseTypeArgs();

  // Expressions, from lowest to highest precedence. parseSequence admits the
  // comma operator; parseExpr, used where a comma separates (arguments, array
  // elements, declarators), does not.
  Expr *parseSequence();
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
