#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// let and const and their declarators.
class VarParser {
public:
  explicit VarParser(Parser &parser);

  VarDeclStmt *parseVarDecl();
  void parseDeclarators(VarDeclStmt *decl);
  bool parseBinding(VarDeclarator &d);
  // Records `const name = "text"` (or a constant named so, declared before
  // it) for stringConstants.
  void recordConstant(llvm::StringRef name, const Expr *init);

private:
  Parser &parser_;
};

} // namespace tz
