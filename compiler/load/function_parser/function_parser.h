#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class FunctionParser {
public:
  explicit FunctionParser(Parser &parser);

  Stmt *parseFunctionDecl(bool isAsync = false);
  FunctionDecl *parseFunction(bool expression, bool isAsync = false);
  bool asyncFunctionAhead();
  Expr *tryAsyncFunction();
  void parseParams(FunctionDecl *fn, Parser::ParamPatterns *patterns = nullptr);
  void addPrologue(FunctionDecl *fn, Parser::ParamPatterns &patterns);
  bool arrowAhead();
  FunctionExpr *parseArrow(std::vector<TypeParamDecl> typeParams = {}, const char *start = nullptr);
  std::vector<TypeParamDecl> parseTypeParams();

private:
  Parser &parser_;
};

}
