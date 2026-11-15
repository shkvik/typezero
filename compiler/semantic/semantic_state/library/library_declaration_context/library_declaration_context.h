#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/builtin_declarations/builtin_declarations.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SMLoc.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

struct LibraryRange {
  const char *begin = nullptr;
  const char *end = nullptr;
};

class LibraryDeclarationContext {
public:
  explicit LibraryDeclarationContext(const BuiltinDeclarations &loaded);

  void rememberLibraryType(TypeAliasDecl *alias);
  void rememberModuleAlias(TypeAliasDecl *alias);
  void rememberLibraryClass(ClassDecl *classDecl);
  void addLibraryRange(const char *begin, const char *end);
  void rememberLibraryFunction(FunctionDecl *functionDecl);
  void addModuleExport(NodeModule module, llvm::StringRef name);


  bool isInsideLibrary(llvm::SMLoc loc) const;
  bool isStreamClass(const ClassDecl *classDecl) const;
  bool isStreamHook(const FunctionDecl *functionDecl) const;
  bool isLinkedStreamHook(const FunctionDecl *functionDecl) const;
  bool isModuleExport(NodeModule module, llvm::StringRef name) const;


  ClassDecl *findLibraryClass(llvm::StringRef name) const;
  ClassDecl *findLibraryClass(LibraryClass libraryClass) const;


  FunctionDecl *findLibraryFunction(llvm::StringRef name) const;
  FunctionDecl *findLibraryFunction(LibraryFunction libraryFunction) const;


  TypeAliasDecl *findLibraryType(llvm::StringRef name) const;
  TypeAliasDecl *findModuleAlias(llvm::StringRef name) const;
  TypeAliasDecl *findLibraryType(LibraryType libraryType) const;


  llvm::ArrayRef<Stmt *> getBuiltinItems(BuiltinText text) const;

private:
  const BuiltinDeclarations &builtins;
  std::vector<LibraryRange> libraryRanges;
  llvm::StringMap<ClassDecl *> libraryClasses;
  llvm::StringMap<TypeAliasDecl *> libraryTypes;
  llvm::StringMap<TypeAliasDecl *> moduleAliases;
  llvm::StringMap<FunctionDecl *> libraryFunctions;
  std::set<std::pair<NodeModule, std::string>> moduleExports;
};

}
