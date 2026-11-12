#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/builtin_declarations/builtin_declarations.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SMLoc.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

struct SourceRange {
  const char *begin = nullptr;
  const char *end = nullptr;
};

class LibraryDeclarations {
public:
  explicit LibraryDeclarations(const BuiltinDeclarations &loaded);

  llvm::ArrayRef<Stmt *> getBuiltinItems(BuiltinText text) const;
  void rememberLibraryFunction(FunctionDecl *functionDecl);
  FunctionDecl *findLibraryFunction(llvm::StringRef name) const;
  void rememberLibraryType(TypeAliasDecl *alias);
  TypeAliasDecl *findLibraryType(llvm::StringRef name) const;
  TypeAliasDecl *findLibraryType(LibraryType libraryType) const;
  void rememberLibraryClass(ClassDecl *classDecl);
  ClassDecl *findLibraryClass(llvm::StringRef name) const;
  ClassDecl *findLibraryClass(LibraryClass libraryClass) const;
  void addLibraryRange(const char *begin, const char *end);
  bool isInsideLibrary(llvm::SMLoc loc) const;
  void rememberModuleAlias(TypeAliasDecl *alias);
  TypeAliasDecl *findModuleAlias(llvm::StringRef name) const;
  void addModuleExport(NodeModule module, llvm::StringRef name);
  bool isModuleExport(NodeModule module, llvm::StringRef name) const;

private:
  const BuiltinDeclarations &builtins;
  llvm::StringMap<FunctionDecl *> libraryFunctions;
  llvm::StringMap<TypeAliasDecl *> libraryTypes;
  llvm::StringMap<ClassDecl *> libraryClasses;
  std::vector<SourceRange> libraryRanges;
  llvm::StringMap<TypeAliasDecl *> moduleAliases;
  std::set<std::pair<NodeModule, std::string>> moduleExports;
};

}
