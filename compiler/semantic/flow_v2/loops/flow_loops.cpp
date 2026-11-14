#include "compiler/semantic/flow_v2/loops/flow_loops.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

FlowLoops::FlowLoops(SemanticState &owner)
    : loopScans(owner),
      loopHeads(owner) {}

LoopScan FlowLoops::scanForOfLoop(const ForOfStmt *loop) const {
  return this->loopScans.scanForOfLoop(loop);
}

LoopScan FlowLoops::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                             llvm::ArrayRef<const Expr *> exprs) const {
  return this->loopScans.scanLoop(stmts, exprs);
}

Flow FlowLoops::loopHead(const LoopScan &scan) const {
  return this->loopHeads.loopHead(scan);
}

Flow FlowLoops::countedLoopHead(const LoopScan &scan, const Expr *cond) const {
  return this->loopHeads.countedLoopHead(scan, cond);
}

}
