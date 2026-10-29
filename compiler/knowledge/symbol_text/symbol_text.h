#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

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
};

}
