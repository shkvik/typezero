#include "compiler/semantic/flow_v2/reachability/flow_reachability.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FlowReachability::FlowReachability(SemanticState &owner)
    : state(owner) {}

bool FlowReachability::isTrueLiteral(const Expr *expr) const {
  auto *literal = llvm::dyn_cast_or_null<BoolLitExpr>(expr);

  return literal != nullptr && literal->value;
}

bool FlowReachability::isBreakingTo(const Stmt *stmt,
                                    llvm::ArrayRef<llvm::StringRef> labels) const {
  if (labels.empty()) {
    return false;
  }
  auto nodes = PlacedNodes::collect({stmt}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (this->isLabeledBreak(placed, labels)) {
      return true;
    }
  }

  return false;
}

bool FlowReachability::isLabeledBreak(const PlacedStmt &placed,
                                      llvm::ArrayRef<llvm::StringRef> labels) const {
  auto *exit = dyn_cast<BreakStmt>(placed.stmt);
  if (placed.place.isInsideFunction() || exit == nullptr) {
    return false;
  }

  return llvm::is_contained(labels, exit->label);
}

bool FlowReachability::isExit(const Stmt *stmt) const {
  auto *statement = dyn_cast<ExprStmt>(stmt);
  if (statement == nullptr) {
    return false;
  }
  auto *call = dyn_cast<CallExpr>(statement->expr);
  bool isStopping = call != nullptr && (call->builtin() == Builtin::ProcessExit ||
                                        call->builtin() == Builtin::LibPanic);

  return isStopping;
}

bool FlowReachability::isAlwaysReturning(const Stmt *stmt,
                                         llvm::ArrayRef<llvm::StringRef> labels) const {
  auto whole = ReturnPart{
      .stmt = stmt,
      .labels = std::vector<llvm::StringRef>(labels.begin(), labels.end()),
  };
  auto answers = std::vector<bool>();
  for (const ReturnPart &part : ChildrenFirst::order(whole, this, &FlowReachability::returnParts)) {
    bool isReturning = this->isNodeReturning(part, answers);
    answers.push_back(isReturning);
  }

  return answers.back();
}

NodeChildren<ReturnPart> FlowReachability::returnParts(ReturnPart part) const {
  if (isa<LabeledStmt>(part.stmt)) {
    return {this->unlabeled(part)};
  }
  auto inner = this->innerStatements(part);
  auto parts = NodeChildren<ReturnPart>();
  for (const Stmt *stmt : inner) {
    parts.push_back(ReturnPart{.stmt = stmt});
  }

  return parts;
}

NodeChildren<const Stmt *> FlowReachability::innerStatements(const ReturnPart &part) const {
  auto *block = dyn_cast<BlockStmt>(part.stmt);
  auto *branch = dyn_cast<IfStmt>(part.stmt);
  auto *cases = dyn_cast<SwitchStmt>(part.stmt);
  if (block != nullptr) {
    return NodeChildren<const Stmt *>(block->body.begin(), block->body.end());
  }
  if (branch != nullptr) {
    return this->ifBranches(branch);
  }
  if (cases != nullptr) {
    return this->switchTailStatements(cases, part.labels);
  }

  return {};
}

NodeChildren<const Stmt *>
FlowReachability::switchTailStatements(const SwitchStmt *switchStmt,
                                       llvm::ArrayRef<llvm::StringRef> labels) const {
  const auto *tail = this->switchTail(switchStmt, labels);
  if (tail == nullptr) {
    return {};
  }

  return NodeChildren<const Stmt *>(tail->begin(), tail->end());
}

ReturnPart FlowReachability::unlabeled(const ReturnPart &part) const {
  auto inner = part;
  auto *labeled = dyn_cast<LabeledStmt>(inner.stmt);
  while (labeled != nullptr) {
    inner.labels.push_back(labeled->label);
    inner.stmt = labeled->body;
    labeled = dyn_cast<LabeledStmt>(inner.stmt);
  }

  return inner;
}

NodeChildren<const Stmt *> FlowReachability::ifBranches(const IfStmt *ifStmt) const {
  auto *literal = dyn_cast<BoolLitExpr>(ifStmt->cond);
  bool isAlwaysThen = literal != nullptr && literal->value && ifStmt->elseStmt == nullptr;
  bool isAlwaysElse = literal != nullptr && !literal->value && isa<EmptyStmt>(ifStmt->thenStmt);
  if (isAlwaysThen) {
    return {ifStmt->thenStmt};
  }
  if (isAlwaysElse) {
    return this->elseBranch(ifStmt);
  }
  if (ifStmt->elseStmt == nullptr) {
    return {};
  }

  return {ifStmt->thenStmt, ifStmt->elseStmt};
}

NodeChildren<const Stmt *> FlowReachability::elseBranch(const IfStmt *ifStmt) const {
  if (ifStmt->elseStmt == nullptr) {
    return {};
  }

  return {ifStmt->elseStmt};
}

const std::vector<Stmt *> *
FlowReachability::switchTail(const SwitchStmt *switchStmt,
                             llvm::ArrayRef<llvm::StringRef> labels) const {
  bool hasDefault = switchStmt->exhaustive || this->hasDefaultCase(switchStmt);
  if (!hasDefault || switchStmt->cases.empty()) {
    return nullptr;
  }
  if (this->hasLeavingCase(switchStmt, labels)) {
    return nullptr;
  }

  return &switchStmt->cases.back().body;
}

bool FlowReachability::hasDefaultCase(const SwitchStmt *switchStmt) const {
  for (const SwitchCase &switchCase : switchStmt->cases) {
    if (switchCase.test == nullptr) {
      return true;
    }
  }

  return false;
}

bool FlowReachability::hasLeavingCase(const SwitchStmt *switchStmt,
                                      llvm::ArrayRef<llvm::StringRef> labels) const {
  auto exits = Exits(JumpOwner::SWITCH, labels);
  for (const SwitchCase &switchCase : switchStmt->cases) {
    if (exits.hasExitIn(switchCase.body)) {
      return true;
    }
  }

  return false;
}

bool FlowReachability::hasLoopExit(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const {
  auto exits = Exits(JumpOwner::LOOP, labels);

  return exits.hasExitIn({body});
}

bool FlowReachability::isNodeReturning(const ReturnPart &part, std::vector<bool> &answers) const {
  const Stmt *stmt = part.stmt;
  llvm::ArrayRef<llvm::StringRef> labels = part.labels;
  switch (stmt->kind()) {
    case Stmt::Kind::Return:
      return true;
    case Stmt::Kind::Expr:
      return this->isExit(stmt);
    case Stmt::Kind::Block:
      return this->isBlockReturning(cast<BlockStmt>(stmt), labels, answers);
    case Stmt::Kind::If:
      return this->isIfReturning(cast<IfStmt>(stmt), labels, answers);
    case Stmt::Kind::While:
      return this->isWhileReturning(cast<WhileStmt>(stmt), labels);
    case Stmt::Kind::DoWhile:
      return this->isDoWhileReturning(cast<DoWhileStmt>(stmt), labels);
    case Stmt::Kind::For:
      return this->isForReturning(cast<ForStmt>(stmt), labels);
    case Stmt::Kind::Switch:
      return this->isSwitchReturning(cast<SwitchStmt>(stmt), labels, answers);
    case Stmt::Kind::Labeled:
      return ChildrenFirst::takeAnswer(answers);
    case Stmt::Kind::VarDecl:
    case Stmt::Kind::Function:
    case Stmt::Kind::ForOf:
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger:
    case Stmt::Kind::Class:
      return false;
  }

  return false;
}

bool FlowReachability::isBlockReturning(const BlockStmt *block,
                                        llvm::ArrayRef<llvm::StringRef> labels,
                                        std::vector<bool> &answers) const {
  auto inner = ChildrenFirst::takeAnswers(answers, block->body.size());

  return llvm::is_contained(inner, true) && !this->isBreakingTo(block, labels);
}

bool FlowReachability::isIfReturning(const IfStmt *ifStmt, llvm::ArrayRef<llvm::StringRef> labels,
                                     std::vector<bool> &answers) const {
  size_t count = this->ifBranches(ifStmt).size();
  auto inner = ChildrenFirst::takeAnswers(answers, count);
  bool isEveryBranch = count > 0 && !llvm::is_contained(inner, false);

  return isEveryBranch && !this->isBreakingTo(ifStmt, labels);
}

bool FlowReachability::isWhileReturning(const WhileStmt *loop,
                                        llvm::ArrayRef<llvm::StringRef> labels) const {
  return this->isTrueLiteral(loop->cond) && !this->hasLoopExit(loop->body, labels);
}

bool FlowReachability::isDoWhileReturning(const DoWhileStmt *loop,
                                          llvm::ArrayRef<llvm::StringRef> labels) const {
  return this->isTrueLiteral(loop->cond) && !this->hasLoopExit(loop->body, labels);
}

bool FlowReachability::isForReturning(const ForStmt *loop,
                                      llvm::ArrayRef<llvm::StringRef> labels) const {
  bool isForever = loop->cond == nullptr || this->isTrueLiteral(loop->cond);

  return isForever && !this->hasLoopExit(loop->body, labels);
}

bool FlowReachability::isSwitchReturning(const SwitchStmt *switchStmt,
                                         llvm::ArrayRef<llvm::StringRef> labels,
                                         std::vector<bool> &answers) const {
  const auto *tail = this->switchTail(switchStmt, labels);
  if (tail == nullptr) {
    return false;
  }
  auto inner = ChildrenFirst::takeAnswers(answers, tail->size());

  return llvm::is_contained(inner, true);
}

}
