#pragma once

#include "compiler/program/ast/module_name/module_name.h"

namespace tz {

struct Module;

struct ImportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  std::string path;
  llvm::SMLoc pathLoc;
  Module *from = nullptr;
  llvm::StringRef whole;
  llvm::SMLoc wholeLoc;
  bool wholeIsDefault = false;
  bool implicit = false;
};

class BuiltinModulePaths {
public:
  static bool isBuiltinModule(llvm::StringRef path);
};

}
