#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class ModuleParser {
public:
  explicit ModuleParser(Parser &parser);

  Stmt *parseImport(bool topLevel);
  Stmt *parseExport(bool topLevel);
  void parseModuleNames(std::vector<ModuleName> &names, bool import);
  bool parseFromPath(std::string &path, llvm::SMLoc &loc, bool from = true);
  void exportDeclared(const Stmt *stmt);

private:
  bool parseImportAttributes(bool json);

  Parser &parser_;
};

}
