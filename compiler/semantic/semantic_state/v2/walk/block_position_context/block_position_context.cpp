#include "compiler/semantic/semantic_state/v2/walk/block_position_context/block_position_context.h"

namespace tz {

void BlockPositionContext::rememberBlockPosition(llvm::ArrayRef<Stmt *> stmts, size_t index) {
  this->block = stmts;
  this->blockIndex = index;
}

void BlockPositionContext::forgetBlock() {
  this->block = {};
}

llvm::ArrayRef<Stmt *> BlockPositionContext::getBlock() const {
  return this->block;
}

size_t BlockPositionContext::getBlockIndex() const {
  return this->blockIndex;
}

void BlockPositionContext::rememberPendingLabels(llvm::ArrayRef<llvm::StringRef> labels) {
  this->pendingLabels.assign(labels.begin(), labels.end());
}

llvm::SmallVector<llvm::StringRef, 1> BlockPositionContext::getPendingLabels() const {
  return this->pendingLabels;
}

llvm::SmallVector<llvm::StringRef, 1> BlockPositionContext::takePendingLabels() {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->pendingLabels);
  this->pendingLabels.clear();

  return labels;
}

void BlockPositionContext::enterJumpTarget(JumpTarget target) {
  this->targets.push_back(std::move(target));
}

JumpTarget BlockPositionContext::leaveJumpTarget() {
  JumpTarget target = std::move(this->targets.back());
  this->targets.pop_back();

  return target;
}

JumpTarget *BlockPositionContext::findJumpTarget(llvm::StringRef label, bool isBreak) {
  for (JumpTarget &target : llvm::reverse(this->targets)) {
    bool isMatching = label.empty() ? (isBreak ? target.kind != JumpTarget::Kind::Block
                                               : target.kind == JumpTarget::Kind::Loop)
                                    : llvm::is_contained(target.labels, label);
    if (isMatching) {
      return &target;
    }
  }

  return nullptr;
}

bool BlockPositionContext::hasLabel(llvm::StringRef label) const {
  for (const JumpTarget &target : this->targets) {
    if (llvm::is_contained(target.labels, label)) {
      return true;
    }
  }

  return false;
}

void BlockPositionContext::addOtherCaseVar(const VarSymbol *variable) {
  this->otherCaseVars.insert(variable);
}

void BlockPositionContext::forgetOtherCaseVar(const VarSymbol *variable) {
  this->otherCaseVars.erase(variable);
}

bool BlockPositionContext::isOtherCaseVar(const VarSymbol *variable) const {
  return this->otherCaseVars.contains(variable);
}

void BlockPositionContext::forgetOtherCaseVars() {
  this->otherCaseVars.clear();
}

BlockSnapshot BlockPositionContext::saveBody() {
  BlockSnapshot saved;
  saved.targets = std::move(this->targets);
  saved.pendingLabels = std::move(this->pendingLabels);
  saved.otherCaseVars = this->otherCaseVars;
  this->targets.clear();
  this->pendingLabels.clear();

  return saved;
}

void BlockPositionContext::restoreBody(BlockSnapshot saved) {
  this->targets = std::move(saved.targets);
  this->pendingLabels = std::move(saved.pendingLabels);
  this->otherCaseVars = std::move(saved.otherCaseVars);
}

}
