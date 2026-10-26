#pragma once

#include "compiler/program/ast/module_name/module_name.h"

namespace tz {

struct Module;

struct ExportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  bool all = false;
  std::string path;
  llvm::SMLoc pathLoc;
  Module *from = nullptr;
};

}
