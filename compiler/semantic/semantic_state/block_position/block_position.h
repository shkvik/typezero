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

class BlockPosition {
public:
  void rememberBlockPosition(llvm::ArrayRef<Stmt *> block, size_t index);
  void forgetBlock();
  llvm::ArrayRef<Stmt *> getBlock() const;
  size_t getBlockIndex() const;
  void rememberFollowing(std::vector<const Stmt *> following);
  std::vector<const Stmt *> takeFollowing();
  void rememberPendingLabels(llvm::ArrayRef<llvm::StringRef> labels);
  llvm::SmallVector<llvm::StringRef, 1> getPendingLabels() const;
  llvm::SmallVector<llvm::StringRef, 1> takePendingLabels();
  void enterJumpTarget(JumpTarget target);
  JumpTarget leaveJumpTarget();
  JumpTarget *findJumpTarget(llvm::StringRef label, bool isBreak);
  bool hasLabel(llvm::StringRef label) const;
  void addOtherCaseVar(const VarSymbol *variable);
  void forgetOtherCaseVar(const VarSymbol *variable);
  bool isOtherCaseVar(const VarSymbol *variable) const;
  void forgetOtherCaseVars();
  BlockSnapshot saveBody();
  void restoreBody(BlockSnapshot saved);

private:
  llvm::ArrayRef<Stmt *> block;
  size_t blockIndex = 0;
  std::vector<const Stmt *> following;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
  std::vector<JumpTarget> targets;
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars;
};

}
