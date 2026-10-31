#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace tz {

struct ParallelPlan {
  struct Task {
    const Node *site = nullptr;
    const FunctionDecl *fn = nullptr;
    int parent = -1;
    bool several = false;
    const AwaitExpr *joinedAt = nullptr;
  };
  struct Piece {
    const FunctionDecl *fn = nullptr;
    const Node *start = nullptr;
    unsigned task = 0;
    bool alone = true;
    std::vector<unsigned> inOrderWith;
    llvm::SMLoc orderedFrom, orderedTo;
    std::vector<llvm::SMLoc> orderedAt;
  };
  std::vector<Task> tasks;
  std::vector<Piece> pieces;
  std::vector<const VarSymbol *> ownedParams;
};

ParallelPlan planParallel(const SourceFile &program, const llvm::SourceMgr &sources);

void explainParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                     llvm::raw_ostream &out);

}
