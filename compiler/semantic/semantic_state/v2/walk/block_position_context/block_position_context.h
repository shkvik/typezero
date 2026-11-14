#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct BlockSnapshot {
  std::vector<JumpTarget> targets;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars;
};

class BlockPositionContext {
public:
  void forgetBlock();
  void forgetOtherCaseVars();
  void restoreBody(BlockSnapshot saved);
  void enterJumpTarget(JumpTarget target);
  void addOtherCaseVar(const VarSymbol *variable);
  void forgetOtherCaseVar(const VarSymbol *variable);
  void rememberPendingLabels(llvm::ArrayRef<llvm::StringRef> labels);
  void rememberBlockPosition(llvm::ArrayRef<Stmt *> block, size_t index);


  bool hasLabel(llvm::StringRef label) const;
  bool isOtherCaseVar(const VarSymbol *variable) const;


  llvm::SmallVector<llvm::StringRef, 1> takePendingLabels();
  llvm::SmallVector<llvm::StringRef, 1> getPendingLabels() const;


  BlockSnapshot saveBody();
  JumpTarget leaveJumpTarget();
  size_t getBlockIndex() const;
  llvm::ArrayRef<Stmt *> getBlock() const;
  JumpTarget *findJumpTarget(llvm::StringRef label, bool isBreak);

private:
  size_t blockIndex = 0;
  llvm::ArrayRef<Stmt *> block;
  std::vector<JumpTarget> targets;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars;
};

}
