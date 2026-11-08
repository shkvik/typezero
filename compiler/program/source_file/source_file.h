#pragma once

#include "compiler/program/ast/export_decl/export_decl.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/import_decl/import_decl.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/builtin_declarations/builtin_declarations.h"
#include "compiler/program/program_inventory/program_inventory.h"
#include "llvm/ADT/DenseMap.h"

namespace tz {

struct Module;
class Type;

struct SourceFile {
  std::vector<Stmt *> items;
  std::vector<ImportDecl> imports;
  std::vector<ExportDecl> exports;
  std::vector<Module *> modules;
  llvm::StringSet<> fieldNames;
  llvm::StringMap<std::vector<std::string>> stringConstants;
  llvm::SmallPtrSet<const FunctionDecl *, 8> unusedFunctions;
  BuiltinDeclarations builtins;
  ProgramInventory inventory;
};

}
