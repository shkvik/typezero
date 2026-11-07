#pragma once

#include "compiler/program/ast/access/access.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class Type;
struct ClassDecl;
struct ClassField;
struct FunctionDecl;
struct VarDeclarator;

class SymbolText {
public:
  static std::string function(const FunctionDecl &fn);
  static std::string field(const ClassField &field);
  static std::string variable(const VarDeclarator &declarator, bool isConst);
  static std::string header(const ClassDecl &cls);
  static std::string collapse(llvm::StringRef text, size_t limit);
  static const char *statementEnd(const char *at, const char *end);

private:
  static std::string typeName(const Type *type);
  static std::string modifiers(Access access, bool isStatic);
  static bool opens(char c);
  static bool closes(char c);
  static bool continues(char c);
  static bool word(char c);
  static bool endsMember(const std::string &out);
  static bool startsMember(char c);
};

}
