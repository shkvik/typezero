#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz::parallel {

class Pieces {
public:
  explicit Pieces(Analysis &analysis);

  void explore();
  unsigned activate(int fn, int root, std::optional<Ref> self, bool callbackStart, bool rootStart);
  int newRoot(int parent, int fn, std::optional<Ref> self, int callerFn, unsigned rec, Mode mode);
  bool scheduled(const Activation &act, unsigned seg) const;
  void findPieces();
  void findHosts();
  std::vector<unsigned> piecesAt(const Activation &act, llvm::ArrayRef<unsigned> segs) const;
  bool once(int fi, unsigned rec, int rootFn, unsigned depth);
  bool fnOnce(int fi, int rootFn, unsigned depth);
  bool rejectionEnds(const AwaitExpr *a, unsigned depth);
  bool fnRejectionEnds(int fi, unsigned depth);
  void findJoins();
  void buildFlow();
  void findBindings();
  bool keeps(const PieceInst &piece, const Access &a) const;
  std::vector<unsigned> reach(int root, const std::vector<unsigned> &from) const;
  std::vector<unsigned> reachAfter(int root,
                                   const std::vector<std::pair<unsigned, llvm::SMLoc>> &from);

private:
  bool ownPiece(unsigned act, unsigned piece) const;
  int stepOf(int fn, llvm::SMLoc loc);
  bool later(int fn, llvm::SMLoc at, llvm::SMLoc origin);
  void addEntry(unsigned act, unsigned piece, const Entry &add, bool &changed);
  void noteFlow(int root, unsigned from, unsigned to, llvm::SMLoc origin);
  Analysis &analysis;
};

}
