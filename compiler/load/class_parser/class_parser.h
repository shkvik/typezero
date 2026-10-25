#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Classes, their members and decorators.
class ClassParser {
public:
  explicit ClassParser(Parser &parser);

  // Classes. A member's modifiers (`static`, `private`...) come before its
  // name; a parameter's before its name in a constructor.
  // With `expression`, a class expression (docs/decorators.md, section 5),
  // whose name may be left out.
  ClassDecl *parseClass(bool expression = false);
  void parseClassMember(ClassDecl *cls);
  // Decorators (docs/decorators.md): `@Name`, `@a.b`, `@Name(args)` or
  // `@(expression)`, as many as there are.
  std::vector<Decorator> parseDecorators();
  Expr *parseDecoratorExpr();
  // Reports decorators where they cannot be: before anything but a class, a
  // member of one or a parameter.
  void rejectDecorators(llvm::ArrayRef<Decorator> decorators);
  bool memberModifierAhead();
  // After an error in a member: skips to the end of it.
  void skipClassMember();
  // The rest of a method after its name: type parameters, parameters, the
  // return type and the body. `start` is where the name starts.
  void parseMethodRest(FunctionDecl *fn, const char *start);
  // `<T, U extends X>` after a name, which tz does not support here.
  void skipTypeParams();
  // `[Symbol.iterator]` is ahead, a member name in brackets.
  bool symbolKeyAhead();

private:
  Parser &parser_;
};

} // namespace tz
