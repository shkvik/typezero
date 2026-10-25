#include "compiler/packages/dependency/dependency.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

bool checkAlias(llvm::StringRef alias, std::string &error) {
  auto validPart = [](llvm::StringRef part) {
    return !part.empty() && !part.starts_with(".") && llvm::all_of(part, [](char c) {
      return llvm::isAlnum(c) || c == '-' || c == '_' || c == '.';
    });
  };
  bool valid;
  if (alias.starts_with("@")) {
    auto [scope, name] = alias.drop_front().split('/');
    valid = validPart(scope) && validPart(name);
  } else {
    valid = validPart(alias);
  }
  if (!valid) {
    error = "'" + alias.str() +
            "' cannot be imported as a package name: write a name like "
            "'zod' or '@std/path'";
    return false;
  }
  if (isBuiltinModule(alias) || alias == "neverthrow") {
    error = "'" + alias.str() +
            "' is the name of a module built into tz; give the package "
            "another name";
    return false;
  }

  return true;
}

} // namespace tz::pkg
