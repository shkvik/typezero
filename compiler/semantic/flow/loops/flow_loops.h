#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/loops/loop_heads/loop_heads.h"
#include "compiler/semantic/flow/loops/loop_scans/loop_scans.h"
#include "compiler/semantic/semantic_state/walk/flow_state_context/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowLoops {
public:
  explicit FlowLoops(SemanticState &owner);

  Flow loopHead(const LoopScan &scan) const;
  Flow countedLoopHead(const LoopScan &scan, const Expr *cond) const;


  LoopScan scanForOfLoop(const ForOfStmt *loop) const;
  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;

private:
  LoopScans loopScans;
  LoopHeads loopHeads;
};

}
