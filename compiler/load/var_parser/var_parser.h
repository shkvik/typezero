#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class VarParser {
public:
  explicit VarParser(Parser &parser);

  VarDeclStmt *parseVarDecl();
  void parseDeclarators(VarDeclStmt *decl);
  bool parseBinding(VarDeclarator &d);
  void recordConstant(llvm::StringRef name, const Expr *init);

private:
  Parser &parser_;
};

}
