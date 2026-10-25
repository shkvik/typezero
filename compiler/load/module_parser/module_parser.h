#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// import and export.
class ModuleParser {
public:
  explicit ModuleParser(Parser &parser);

  // Imports and exports go to imports_ and exports_; what stays among the
  // statements is the declaration after `export`, or an empty statement.
  Stmt *parseImport(bool topLevel);
  Stmt *parseExport(bool topLevel);
  // `{ a, b as c }`, with the lexer at `{`.
  void parseModuleNames(std::vector<ModuleName> &names, bool import);
  // `from "./x"`, or just the path without `from`; false after reporting an
  // error.
  bool parseFromPath(std::string &path, llvm::SMLoc &loc, bool from = true);
  // Records the names a declaration after `export` declares.
  void exportDeclared(const Stmt *stmt);

private:
  Parser &parser_;
};

} // namespace tz
