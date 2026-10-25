#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Functions: declarations, expressions, arrows, async, parameters and type parameters.
class FunctionParser {
public:
  explicit FunctionParser(Parser &parser);

  Stmt *parseFunctionDecl(bool isAsync = false);
  // A function, or a declaration without a body, such as an overload
  // signature, which parseFunctionDecl rejects. A function expression may
  // have no name.
  FunctionDecl *parseFunction(bool expression, bool isAsync = false);
  // `async function` here, on one line.
  bool asyncFunctionAhead();
  // At `async`: an async arrow or function expression, or null if `async`
  // is just a name here (nothing is consumed then).
  Expr *tryAsyncFunction();
  void parseParams(FunctionDecl *fn, Parser::ParamPatterns *patterns = nullptr);
  void addPrologue(FunctionDecl *fn, Parser::ParamPatterns &patterns);
  // Whether the `(` here starts the parameters of an arrow function.
  bool arrowAhead();
  // `x => body` or `(params): T => body`; a generic arrow `<T>(x: T) => x`
  // has its type parameters already parsed.
  FunctionExpr *parseArrow(std::vector<TypeParamDecl> typeParams = {}, const char *start = nullptr);
  // `<T, U extends number = int>`
  std::vector<TypeParamDecl> parseTypeParams();

private:
  Parser &parser_;
};

} // namespace tz
