#include "compiler/semantic/semantic_state/block_position/block_position.h"

namespace tz {

void BlockPosition::rememberBlockPosition(llvm::ArrayRef<Stmt *> stmts, size_t index) {
  this->block = stmts;
  this->blockIndex = index;
}

void BlockPosition::forgetBlock() {
  this->block = {};
}

llvm::ArrayRef<Stmt *> BlockPosition::getBlock() const {
  return this->block;
}

size_t BlockPosition::getBlockIndex() const {
  return this->blockIndex;
}

void BlockPosition::rememberFollowing(std::vector<const Stmt *> stmts) {
  this->following = std::move(stmts);
}

std::vector<const Stmt *> BlockPosition::takeFollowing() {
  std::vector<const Stmt *> stmts = std::move(this->following);
  this->following.clear();

  return stmts;
}

void BlockPosition::rememberPendingLabels(llvm::ArrayRef<llvm::StringRef> labels) {
  this->pendingLabels.assign(labels.begin(), labels.end());
}

llvm::SmallVector<llvm::StringRef, 1> BlockPosition::getPendingLabels() const {
  return this->pendingLabels;
}

llvm::SmallVector<llvm::StringRef, 1> BlockPosition::takePendingLabels() {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->pendingLabels);
  this->pendingLabels.clear();

  return labels;
}

void BlockPosition::enterJumpTarget(JumpTarget target) {
  this->targets.push_back(std::move(target));
}

JumpTarget BlockPosition::leaveJumpTarget() {
  JumpTarget target = std::move(this->targets.back());
  this->targets.pop_back();

  return target;
}

JumpTarget *BlockPosition::findJumpTarget(llvm::StringRef label, bool isBreak) {
  for (JumpTarget &target : llvm::reverse(this->targets)) {
    bool matches = label.empty() ? (isBreak ? target.kind != JumpTarget::Kind::Block
                                            : target.kind == JumpTarget::Kind::Loop)
                                 : llvm::is_contained(target.labels, label);
    if (matches) {
      return &target;
    }
  }

  return nullptr;
}

bool BlockPosition::hasLabel(llvm::StringRef label) const {
  for (const JumpTarget &target : this->targets) {
    if (llvm::is_contained(target.labels, label)) {
      return true;
    }
  }

  return false;
}

void BlockPosition::addOtherCaseVar(const VarSymbol *var) {
  this->otherCaseVars.insert(var);
}

void BlockPosition::forgetOtherCaseVar(const VarSymbol *var) {
  this->otherCaseVars.erase(var);
}

bool BlockPosition::isOtherCaseVar(const VarSymbol *var) const {
  return this->otherCaseVars.contains(var);
}

void BlockPosition::forgetOtherCaseVars() {
  this->otherCaseVars.clear();
}

BlockSnapshot BlockPosition::saveBody() {
  BlockSnapshot saved;
  saved.targets = std::move(this->targets);
  saved.pendingLabels = std::move(this->pendingLabels);
  saved.otherCaseVars = this->otherCaseVars;
  this->targets.clear();
  this->pendingLabels.clear();

  return saved;
}

void BlockPosition::restoreBody(BlockSnapshot saved) {
  this->targets = std::move(saved.targets);
  this->pendingLabels = std::move(saved.pendingLabels);
  this->otherCaseVars = std::move(saved.otherCaseVars);
}

}
