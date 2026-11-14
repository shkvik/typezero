#include "compiler/semantic/code/loops/plain_loops/plain_loops.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

PlainLoops::PlainLoops(SemanticState &owner)
    : state(owner) {}

void PlainLoops::checkJump(const CheckJump &task) {
  auto jumped = this->checkJumpTarget(task.stmt);
  if (jumped.isErr()) {
    return this->state.diagnostics.error(task.stmt->loc(), jumped.error().message);
  }
}

PlainLoops::ResultJumpTargeted PlainLoops::checkJumpTarget(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  auto label = this->jumpLabel(stmt);
  auto *target = this->state.walk.block.findJumpTarget(label, isBreak);
  auto found = this->checkTargetFound(target, label, isBreak);
  if (found.isErr()) {
    return err(found.error());
  }
  auto looped = this->checkContinueTarget(*target, label, isBreak);
  if (looped.isErr()) {
    return err(looped.error());
  }
  if (isBreak && target->kind != JumpTarget::Kind::Loop) {
    const auto &here = this->state.walk.flow.getFlow();
    target->breaks = this->state.flow.joins.join(target->breaks, here);
  }

  return ok(stmt);
}

void PlainLoops::walkLabeled(const WalkLabeled &task) {
  auto labels = llvm::SmallVector<llvm::StringRef, 1>();
  Stmt *body = task.labeled;
  auto *labeled = dyn_cast<LabeledStmt>(body);
  while (labeled != nullptr) {
    auto fresh = this->checkLabelFresh(labels, labeled);
    if (fresh.isErr()) {
      this->state.diagnostics.error(labeled->loc(), fresh.error().message);
    }
    labels.push_back(labeled->label);
    body = labeled->body;
    labeled = dyn_cast<LabeledStmt>(body);
  }
  if (this->isLoop(body) || isa<SwitchStmt>(body)) {
    this->state.walk.block.rememberPendingLabels(labels);
    return this->state.walk.tasks.pushTask(WalkStmt{.stmt = body});
  }
  auto labelable = this->checkLabelBody(body);
  if (labelable.isErr()) {
    return this->state.diagnostics.error(body->loc(), labelable.error().message);
  }
  this->state.walk.block.rememberPendingLabels(labels);
  this->pushTarget(JumpTarget::Kind::Block);
  this->state.walk.tasks.pushTask(FinishLabeledBlock{});

  return this->state.walk.tasks.pushTask(WalkStmt{.stmt = body});
}

void PlainLoops::finishLabeledBlock() {
  auto breaks = this->state.walk.block.leaveJumpTarget().breaks;
  const auto &after = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(after, breaks);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

void PlainLoops::startScannedBody(Stmt *body, const LoopScan &scan) {
  this->pushTarget(JumpTarget::Kind::Loop);
  if (scan.hasAwait) {
    this->state.walk.functions.enterAwaitLoop();
  }
  this->state.walk.tasks.pushTask(LeaveLoopBody{.isAwait = scan.hasAwait});

  return this->state.walk.tasks.pushTask(WalkBody{.body = body});
}

void PlainLoops::leaveLoopBody(const LeaveLoopBody &task) {
  if (task.isAwait) {
    this->state.walk.functions.leaveAwaitLoop();
  }
  this->state.walk.block.leaveJumpTarget();
}

void PlainLoops::restoreLoopFlow(const RestoreLoopFlow &task) {
  return this->state.walk.flow.replaceFlow(task.flow);
}

void PlainLoops::walkWhile(const WalkWhile &task) {
  WhileStmt *stmt = task.whileStmt;
  auto scan = this->state.flow.loops.scanLoop({stmt->body}, {stmt->cond});
  auto head = this->state.flow.loops.countedLoopHead(scan, stmt->cond);
  this->state.walk.flow.replaceFlow(head);
  this->state.walk.tasks.pushTask(AfterWhileCondition{
      .whileStmt = stmt,
      .scan = std::move(scan),
      .head = std::move(head),
  });

  return this->state.walk.tasks.pushTask(CheckCondition{.cond = stmt->cond});
}

void PlainLoops::afterWhileCondition(const AfterWhileCondition &task) {
  WhileStmt *stmt = task.whileStmt;
  stmt->cond = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  this->state.walk.flow.replaceFlow(flows.whenTrue);
  auto after = this->flowAfter(task.scan, flows.whenFalse, task.head);
  this->state.walk.tasks.pushTask(RestoreLoopFlow{.flow = std::move(after)});

  return this->startScannedBody(stmt->body, task.scan);
}

void PlainLoops::walkDoWhile(const WalkDoWhile &task) {
  DoWhileStmt *stmt = task.doWhile;
  auto scan = this->state.flow.loops.scanLoop({stmt->body}, {stmt->cond});
  auto head = this->state.flow.loops.loopHead(scan);
  this->state.walk.flow.replaceFlow(head);
  this->state.walk.tasks.pushTask(AfterDoWhileBody{.doWhile = stmt, .scan = scan, .head = head});

  return this->startScannedBody(stmt->body, scan);
}

void PlainLoops::afterDoWhileBody(const AfterDoWhileBody &task) {
  const Flow &bodyEnd = this->state.walk.flow.getFlow();
  auto beforeCond = this->state.flow.joins.join(bodyEnd, task.head);
  this->state.walk.flow.replaceFlow(std::move(beforeCond));
  this->state.walk.tasks.pushTask(
      AfterDoWhileCondition{.doWhile = task.doWhile, .scan = task.scan, .head = task.head});

  return this->state.walk.tasks.pushTask(CheckCondition{.cond = task.doWhile->cond});
}

void PlainLoops::afterDoWhileCondition(const AfterDoWhileCondition &task) {
  task.doWhile->cond = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  auto after = this->flowAfter(task.scan, flows.whenFalse, task.head);

  return this->state.walk.flow.replaceFlow(std::move(after));
}

void PlainLoops::walkFor(const WalkFor &task) {
  ForStmt *stmt = task.forStmt;
  auto labels = this->state.walk.block.getPendingLabels();
  this->state.walk.scopes.openScope();
  this->state.walk.block.forgetBlock();
  this->state.walk.tasks.pushTask(AfterForInit{.forStmt = stmt, .labels = std::move(labels)});
  if (stmt->init != nullptr) {
    this->state.walk.tasks.pushTask(WalkStmt{.stmt = stmt->init});
  }
}

void PlainLoops::afterForInit(const AfterForInit &task) {
  ForStmt *stmt = task.forStmt;
  auto scan = this->state.flow.loops.scanLoop({stmt->body}, {stmt->cond, stmt->step});
  auto head = this->state.flow.loops.countedLoopHead(scan, stmt->cond);
  this->state.walk.flow.replaceFlow(head);
  auto whenFalse = head;
  whenFalse.unreachable = true;
  auto draft = ForDraft{
      .forStmt = stmt,
      .labels = task.labels,
      .scan = std::move(scan),
      .head = head,
      .whenTrue = head,
      .whenFalse = std::move(whenFalse),
  };
  if (stmt->cond == nullptr) {
    return this->startForBody(std::move(draft));
  }
  this->state.walk.tasks.pushTask(AfterForCondition{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(CheckCondition{.cond = stmt->cond});
}

void PlainLoops::afterForCondition(const AfterForCondition &task) {
  auto draft = task.draft;
  draft.forStmt->cond = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  draft.whenTrue = std::move(flows.whenTrue);
  draft.whenFalse = std::move(flows.whenFalse);

  return this->startForBody(std::move(draft));
}

void PlainLoops::startForBody(ForDraft draft) {
  Stmt *body = draft.forStmt->body;
  auto scan = draft.scan;
  this->state.walk.flow.replaceFlow(draft.whenTrue);
  this->state.walk.tasks.pushTask(AfterForBody{.draft = std::move(draft)});

  return this->startScannedBody(body, scan);
}

void PlainLoops::afterForBody(const AfterForBody &task) {
  ForStmt *stmt = task.draft.forStmt;
  this->state.walk.tasks.pushTask(FinishFor{.draft = task.draft});
  if (stmt->step == nullptr) {
    return;
  }
  this->enterStepFlow(stmt->body, task.draft.labels, task.draft.head, task.draft.whenTrue);
  auto *yield = dyn_cast<YieldExpr>(stmt->step);
  if (yield != nullptr) {
    yield->discarded = true;
  }
  this->state.walk.tasks.pushTask(AfterForStep{.forStmt = stmt});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &stmt->step, .expected = nullptr});
}

void PlainLoops::afterForStep(const AfterForStep &task) {
  return this->state.walk.tasks.pushTask(CheckUsedValue{.expr = task.forStmt->step});
}

void PlainLoops::finishFor(const FinishFor &task) {
  auto after = this->flowAfter(task.draft.scan, task.draft.whenFalse, task.draft.head);
  this->state.walk.flow.replaceFlow(std::move(after));

  return this->state.walk.tasks.pushTask(CloseScope{});
}

bool PlainLoops::isLoop(const Stmt *stmt) const {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

void PlainLoops::pushTarget(JumpTarget::Kind kind) {
  auto labels = this->state.walk.block.takePendingLabels();
  auto breaks = Flow();
  breaks.unreachable = true;
  auto target = JumpTarget{.kind = kind, .labels = std::move(labels), .breaks = std::move(breaks)};
  this->state.walk.block.enterJumpTarget(std::move(target));
}

llvm::StringRef PlainLoops::jumpLabel(const Stmt *stmt) const {
  auto *exit = dyn_cast<BreakStmt>(stmt);
  if (exit != nullptr) {
    return exit->label;
  }

  return cast<ContinueStmt>(stmt)->label;
}

PlainLoops::ResultTargetFound
PlainLoops::checkTargetFound(const JumpTarget *target, llvm::StringRef label, bool isBreak) const {
  if (target == nullptr && !label.empty()) {
    auto message = ErrorTextJump::noLabel(label);

    return err(CodeLoopsRegistryCheckJumpError{
        .code = CodeLoopsRegistryCheckJumpErrorCode::LABEL_NOT_FOUND,
        .message = message,
    });
  }
  if (target == nullptr && isBreak) {
    auto message = ErrorTextJump::breakOut();

    return err(CodeLoopsRegistryCheckJumpError{
        .code = CodeLoopsRegistryCheckJumpErrorCode::BREAK_OUTSIDE,
        .message = message,
    });
  }
  if (target == nullptr) {
    auto message = ErrorTextJump::continueOut();

    return err(CodeLoopsRegistryCheckJumpError{
        .code = CodeLoopsRegistryCheckJumpErrorCode::CONTINUE_OUTSIDE,
        .message = message,
    });
  }

  return ok(target);
}

PlainLoops::ResultTargetFound PlainLoops::checkContinueTarget(const JumpTarget &target,
                                                              llvm::StringRef label,
                                                              bool isBreak) const {
  if (!isBreak && target.kind != JumpTarget::Kind::Loop) {
    auto message = ErrorTextJump::notLoop(KeywordTable::getName(Keyword::CONTINUE_KEYWORD), label);

    return err(CodeLoopsRegistryCheckJumpError{
        .code = CodeLoopsRegistryCheckJumpErrorCode::CONTINUE_NOT_LOOP,
        .message = message,
    });
  }

  return ok(&target);
}

PlainLoops::ResultLabelIsFresh PlainLoops::checkLabelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                                           const LabeledStmt *labeled) const {
  bool isDuplicate =
      llvm::is_contained(labels, labeled->label) || this->state.walk.block.hasLabel(labeled->label);
  if (isDuplicate) {
    auto message = ErrorTextLabel::duplicate(labeled->label);

    return err(CodeLoopsRegistryLabelFreshError{
        .code = CodeLoopsRegistryLabelFreshErrorCode::DUPLICATE_LABEL,
        .message = message,
    });
  }

  return ok(labeled);
}

PlainLoops::ResultBodyCanLabel PlainLoops::checkLabelBody(const Stmt *body) const {
  bool isDeclaration = isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) ||
                       isa<TypeAliasDecl>(body) || isa<EnumDecl>(body);
  if (isDeclaration) {
    auto message = ErrorTextLabelBody::declared();

    return err(CodeLoopsRegistryLabelBodyError{
        .code = CodeLoopsRegistryLabelBodyErrorCode::DECLARATION_LABELED,
        .message = message,
    });
  }

  return ok(body);
}

bool PlainLoops::isContinuingTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *next = dyn_cast<ContinueStmt>(placed.stmt);
    bool isOwn = next != nullptr && !placed.place.isInsideFunction();
    if (isOwn && this->isContinuingLoop(next, placed.place, labels)) {
      return true;
    }
  }

  return false;
}

bool PlainLoops::isContinuingLoop(const ContinueStmt *next, const TreePlace &place,
                                  llvm::ArrayRef<llvm::StringRef> labels) const {
  if (!next->label.empty()) {
    return llvm::is_contained(labels, next->label);
  }

  return !place.isInsideLoop();
}

Flow PlainLoops::flowAfter(const LoopScan &scan, const Flow &whenFalse, const Flow &head) const {
  if (scan.isBreakingOut) {
    return this->state.flow.joins.join(whenFalse, head);
  }

  return whenFalse;
}

void PlainLoops::enterStepFlow(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels,
                               const Flow &head, const Flow &whenTrue) {
  if (this->isContinuingTo(body, labels)) {
    this->state.walk.flow.replaceFlow(head);
    return;
  }
  if (this->state.walk.flow.isUnreachable()) {
    this->state.walk.flow.replaceFlow(whenTrue);
  }
}

}
