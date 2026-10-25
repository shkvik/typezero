#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace tz {

// What may run in parallel in a program checked by Checker (docs/async.md,
// sections 2-5): the pieces of its async code, what each reads and writes,
// and which pieces keep node's order because they touch the same data. The
// parallel runtime follows these decisions (runtime/core/parallel/parallel.c,
// Lowering: the pieces that may run ahead, and the places where they wait for
// their turn); tz build --explain-parallel explains them.
struct ParallelPlan {
  // A task: the top-level code, a call of an async function that nothing
  // awaits right away, or a callback that runs on its own later (of then,
  // of a timer). It runs its pieces one after another.
  struct Task {
    // What starts it: the call or the function given; null for the
    // top-level code, which is the first task.
    const Node *site = nullptr;
    const FunctionDecl *fn = nullptr; // null for the top-level code
    int parent = -1;
    // Several copies of it may run at once: it is started in a loop, by a
    // callback of map, again and again by setInterval...
    bool several = false;
    // The `await Promise.all(...)` (or allSettled) that waits for it, before
    // which it ends; null if it may outlive the task that started it.
    const AwaitExpr *joinedAt = nullptr;
  };
  // A piece: the code of `fn` (null for the top-level code) from `start` to
  // the next await, in a task. `start` is an AwaitExpr, the ForOfStmt of a
  // `for await`, the YieldExpr of a `yield*` in an async generator, or null
  // for the start of the function.
  struct Piece {
    const FunctionDecl *fn = nullptr;
    const Node *start = nullptr;
    unsigned task = 0;
    // No other piece may run at the same time.
    bool alone = true;
    // The pieces that may run at the same time and touch data it touches,
    // one of the two writing: they keep node's order. Only the part from
    // the first to the last access of that data does.
    std::vector<unsigned> inOrderWith;
    llvm::SMLoc orderedFrom, orderedTo;
    // Each place where it touches that data: the access in the piece's own
    // code, or the call that makes it. The parallel runtime waits for the
    // piece's turn before the first of them it comes to (tz_order_enter).
    std::vector<llvm::SMLoc> orderedAt;
  };
  std::vector<Task> tasks;
  std::vector<Piece> pieces;
};

// Finds the pieces of `program` and decides what may run in parallel.
ParallelPlan planParallel(const SourceFile &program, const llvm::SourceMgr &sources);

// The same, explained as text: for each async function and callback, each
// piece, what it reads and writes, and with which other pieces it keeps
// node's order and why (see README, "Async").
void explainParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                     llvm::raw_ostream &out);

} // namespace tz
