#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/IR/DIBuilder.h"

namespace tz {

class DebugTypes;

class DebugInfo {
public:
  explicit DebugInfo(Lowering &lowering);
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

  Lowering &lowering_;
  std::unique_ptr<llvm::DIBuilder> builder_;
  std::unique_ptr<DebugTypes> types_;
  llvm::DenseSet<std::pair<const llvm::Function *, const VarSymbol *>> declared_;
  llvm::DenseMap<const llvm::Function *, const FunctionDecl *> owners_;
  llvm::DICompileUnit *unit_ = nullptr;
  llvm::DISubroutineType *signature_ = nullptr;
  llvm::DenseMap<unsigned, std::string> paths_;
  llvm::DenseMap<unsigned, llvm::DIFile *> files_;
  llvm::DenseMap<std::pair<llvm::DISubprogram *, llvm::DIFile *>, llvm::DIScope *> scopes_;
  unsigned entryBuffer_ = 0;
};

}
