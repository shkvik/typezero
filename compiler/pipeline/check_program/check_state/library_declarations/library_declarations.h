#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SMLoc.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

enum class NodeModule {
  Fs,
  FsPromises,
  Path,
  PathPosix,
  PathWin32,
  Os,
  Process,
  Buffer,
  TzIo,
  TzFs,
  TzCrypto,
  TimersPromises,
  Readline,
  ReadlinePromises,
  ChildProcess,
  Crypto,
};

struct SourceRange {
  const char *begin = nullptr;
  const char *end = nullptr;
};

class LibraryDeclarations {
public:
  void rememberLibraryFunction(FunctionDecl *fn);
  FunctionDecl *findLibraryFunction(llvm::StringRef name) const;
  void rememberLibraryType(TypeAliasDecl *alias);
  TypeAliasDecl *findLibraryType(llvm::StringRef name) const;
  void rememberLibraryClass(ClassDecl *cls);
  ClassDecl *findLibraryClass(llvm::StringRef name) const;
  void addLibraryRange(const char *begin, const char *end);
  bool isInsideLibrary(llvm::SMLoc loc) const;
  void rememberModuleAlias(TypeAliasDecl *alias);
  TypeAliasDecl *findModuleAlias(llvm::StringRef name) const;
  bool hasModuleAlias(llvm::StringRef name) const;
  void addModuleExport(NodeModule module, llvm::StringRef name);
  bool isModuleExport(NodeModule module, llvm::StringRef name) const;

private:
  llvm::StringMap<FunctionDecl *> libraryFunctions;
  llvm::StringMap<TypeAliasDecl *> libraryTypes;
  llvm::StringMap<ClassDecl *> libraryClasses;
  std::vector<SourceRange> libraryRanges;
  llvm::StringMap<TypeAliasDecl *> moduleAliases;
  std::set<std::pair<NodeModule, std::string>> moduleExports;
};

}
