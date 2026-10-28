#pragma once

#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/program/source_file/source_file.h"
#include <memory>

namespace tz {

struct Module {
  std::string path;
  std::string name;
  SourceFile file;
  bool isEntry = false;
  bool builtin = false;
  std::vector<unsigned> buffers;
  unsigned order = 0;
  std::vector<Module *> loadPath;
  std::unique_ptr<DeclarationFile> declaration;
};

}
