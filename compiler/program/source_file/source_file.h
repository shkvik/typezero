#pragma once

#include "compiler/program/ast/export_decl/export_decl.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/import_decl/import_decl.h"
#include "compiler/program/ast/stmt/stmt.h"

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
  bool usesAsync = false;
  bool topLevelAwait = false;
  bool comparesFunctions = false;
  bool usesTls = false;
  const Type *systemError = nullptr;
  llvm::SmallPtrSet<const FunctionDecl *, 8> unusedFunctions;
  std::vector<FunctionDecl *> instances;
  std::vector<const Type *> subtypes;
  std::vector<std::pair<const Type *, const Type *>> views;
};

}
