#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class ClassParser {
public:
  explicit ClassParser(Parser &parser);

  ClassDecl *parseClass(bool expression = false);
  void parseClassMember(ClassDecl *cls);
  std::vector<Decorator> parseDecorators();
  Expr *parseDecoratorExpr();
  void rejectDecorators(llvm::ArrayRef<Decorator> decorators);
  bool memberModifierAhead();
  void skipClassMember();
  void parseMethodRest(FunctionDecl *fn, const char *start);
  void skipTypeParams();
  bool symbolKeyAhead();

private:
  Parser &parser_;
};

}
