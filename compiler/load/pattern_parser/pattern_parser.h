#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Destructuring patterns, expanded into plain declarations and assignments through
// hidden variables.
class PatternParser {
public:
  explicit PatternParser(Parser &parser);

  // At `[` or `{`.
  std::unique_ptr<Parser::Pattern> parseBindingPattern();
  std::unique_ptr<Parser::Pattern> parseObjectPattern();
  // The pattern an array or object literal on the left of `=` stands for.
  std::unique_ptr<Parser::Pattern> patternFromLiteral(Expr *lit);
  // Declares the variables of `pattern`, taking their values from `value`,
  // which a hidden variable holds.
  void declarePattern(VarDeclStmt *decl, const Parser::Pattern &pattern, Expr *value,
                      std::optional<TypeRef> typeAnn);
  // The same from the array or object in the variable `base`.
  void declareElements(VarDeclStmt *decl, const Parser::Pattern &pattern, llvm::StringRef base);
  // `[a, b] = value` or `({ a, b } = value)` as a statement.
  Stmt *assignPattern(llvm::SMLoc loc, const Parser::Pattern &pattern, Expr *value);
  // The value of element `i` of the array in `base`: `base[i]`, with the
  // default value, or `base.slice(i)` for the rest; of an object pattern,
  // `base.key` or `base[key]`, or for the rest a copy of the object without
  // the other fields.
  Expr *elementOf(llvm::StringRef base, const Parser::Pattern &pattern, size_t i,
                  std::vector<const IndexExpr *> &keyReads);
  VarDeclarator hiddenDeclarator(llvm::SMLoc loc, Expr *init);

private:
  Parser &parser_;
};

} // namespace tz
