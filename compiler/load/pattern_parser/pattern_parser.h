#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class PatternParser {
public:
  explicit PatternParser(Parser &parser);

  std::unique_ptr<Parser::Pattern> parseBindingPattern();
  std::unique_ptr<Parser::Pattern> parseObjectPattern();
  std::unique_ptr<Parser::Pattern> patternFromLiteral(Expr *lit);
  void declarePattern(VarDeclStmt *decl, const Parser::Pattern &pattern, Expr *value,
                      std::optional<TypeRef> typeAnn);
  void declareElements(VarDeclStmt *decl, const Parser::Pattern &pattern, llvm::StringRef base);
  Stmt *assignPattern(llvm::SMLoc loc, const Parser::Pattern &pattern, Expr *value);
  Expr *elementOf(llvm::StringRef base, const Parser::Pattern &pattern, size_t i,
                  std::vector<const IndexExpr *> &keyReads);
  VarDeclarator hiddenDeclarator(llvm::SMLoc loc, Expr *init);

private:
  Parser &parser_;
};

}
