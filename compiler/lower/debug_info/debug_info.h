#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/IR/DIBuilder.h"

namespace tz {

class DebugTypes;

class DebugInfo {
public:
  explicit DebugInfo(Lowering &owner);
  ~DebugInfo();

  void start(const SourceFile &file);
  void beginFunction(llvm::Function *function, const FunctionDecl *fn);
  void beginTopLevel(llvm::Function *function);
  void at(llvm::SMLoc loc);
  llvm::DebugLoc here() const;
  void restore(const llvm::DebugLoc &loc);
  void declare(const VarSymbol *var, llvm::Value *address);
  void declareGlobal(const VarSymbol *var, llvm::Value *address);
  void finish();

private:
  llvm::DIFile *fileOf(unsigned buffer);
  llvm::DIScope *scopeIn(llvm::DISubprogram *subprogram, llvm::DIFile *file);
  llvm::DISubprogram *describe(llvm::Function *function, llvm::StringRef name, llvm::SMLoc loc);
  void repair(llvm::Function &function);
  unsigned argumentOf(const VarSymbol *var) const;

  Lowering &lowering;
  std::unique_ptr<llvm::DIBuilder> builder;
  std::unique_ptr<DebugTypes> types;
  llvm::DenseSet<std::pair<const llvm::Function *, const VarSymbol *>> declared;
  llvm::DenseMap<const llvm::Function *, const FunctionDecl *> owners;
  llvm::DICompileUnit *unit = nullptr;
  llvm::DISubroutineType *signature = nullptr;
  llvm::DenseMap<unsigned, std::string> paths;
  llvm::DenseMap<unsigned, llvm::DIFile *> files;
  llvm::DenseMap<std::pair<llvm::DISubprogram *, llvm::DIFile *>, llvm::DIScope *> scopes;
  unsigned entryBuffer = 0;
};

}
