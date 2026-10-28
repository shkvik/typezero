#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <string>
#include <vector>

namespace tz {

namespace pkg {
struct Graph;
}

class ModuleResolver;
class LibraryModules;

class Loader {
public:
  Loader(llvm::SourceMgr &sourceMgr, AstContext &ctx, Diagnostics &diags,
         const pkg::Graph *packages);
  bool run(llvm::StringRef entryPath, SourceFile &program);

private:
  enum class State { New, Loading, Loaded };
  Module *parse(const std::string &filePath, const std::string &realPath, std::string name,
                std::string &error);
  void visit(Module *module);
  std::string relativeName(llvm::StringRef realPath) const;
  void loadDeclarations(Module *module, llvm::StringRef filePath, bool library);
  void rejectReferences(llvm::StringRef text);
  llvm::SourceMgr &sourceMgr_;
  AstContext &ctx_;
  Diagnostics &diags_;
  const pkg::Graph *packages_;
  llvm::DenseMap<const Module *, int> packageOf_;
  std::string entryDir_;
  std::string entryRealDir_;
  llvm::StringMap<Module *> byRealPath_;
  llvm::StringMap<Module *> libraries_;
  llvm::DenseMap<const Module *, std::string> realPaths_;
  llvm::DenseMap<const Module *, State> states_;
  std::vector<Module *> loading_;
  std::vector<Module *> order_;

public:
  ~Loader();

  ModuleResolver &resolver() const { return *resolver_; }
  LibraryModules &library() const { return *library_; }

private:
  friend class ModuleResolver;
  friend class LibraryModules;
  std::unique_ptr<ModuleResolver> resolver_;
  std::unique_ptr<LibraryModules> library_;
};

bool loadProgram(llvm::StringRef entryPath, llvm::SourceMgr &sourceMgr, AstContext &ctx,
                 Diagnostics &diags, SourceFile &program, const pkg::Graph *packages = nullptr);

}
