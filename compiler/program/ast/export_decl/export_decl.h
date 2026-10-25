#pragma once

#include "compiler/program/ast/module_name/module_name.h"

namespace tz {

struct Module;

// `export { a, b as c }`, `export { a } from "./x"` or `export * from "./x"`.
// `export` before a declaration is an export of the names it declares.
struct ExportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  bool all = false; // `export *`
  std::string path; // empty when the names are this file's own
  llvm::SMLoc pathLoc;
  Module *from = nullptr; // set by the loader
};

} // namespace tz
