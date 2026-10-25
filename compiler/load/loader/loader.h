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

// Loads the files of a program: see loadProgram.
class ModuleResolver;
class LibraryModules;

class Loader {
public:
  Loader(llvm::SourceMgr &sourceMgr, AstContext &ctx, Diagnostics &diags,
         const pkg::Graph *packages);
  bool run(llvm::StringRef entryPath, SourceFile &program);

private:
  enum class State { New, Loading, Loaded };
  // Reads and parses a file. Null if it cannot be read.
  Module *parse(const std::string &filePath, const std::string &realPath, std::string name,
                std::string &error);
  // Loads the files `module` imports, then puts it in the order.
  void visit(Module *module);
  // `realPath` relative to the entry's directory, with '/'.
  std::string relativeName(llvm::StringRef realPath) const;
  // `/// <reference path="...">` before the code: TypeScript's way to join
  // files without modules.
  void rejectReferences(llvm::StringRef text);
  llvm::SourceMgr &sourceMgr_;
  AstContext &ctx_;
  Diagnostics &diags_;
  // The project's packages (packages[0] is the project), or null without a
  // tz.json.
  const pkg::Graph *packages_;
  llvm::DenseMap<const Module *, int> packageOf_; // -1 without a tz.json
  std::string entryDir_;                          // as given
  std::string entryRealDir_;                      // canonical
  llvm::StringMap<Module *> byRealPath_;
  llvm::StringMap<Module *> libraries_; // by name, null for one that cannot be read
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

// Reads the entry file of a program and every file it imports, directly or
// not, and joins them into one program: `program.modules` lists the files in
// the order they load, as in JavaScript (what a file imports first, in the
// order of its imports, then the file itself; each file once), and
// `program.items` holds their statements in that order. Import paths are
// relative: "./math" and "./math.tz" are math.tz, and a directory is its
// index.tz (docs/modules.md). A name that is not a path is
// a package of `packages`, the project of the entry file and what it depends
// on (docs/packages.md); without a tz.json there are none. Problems with
// imports are reported to `diags`; returns false only if the entry file
// cannot be read.
bool loadProgram(llvm::StringRef entryPath, llvm::SourceMgr &sourceMgr, AstContext &ctx,
                 Diagnostics &diags, SourceFile &program, const pkg::Graph *packages = nullptr);

} // namespace tz
