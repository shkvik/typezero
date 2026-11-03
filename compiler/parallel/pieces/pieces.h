#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class Pieces {
public:
  explicit Pieces(Analysis &analysis);

  void explore();
  unsigned activate(unsigned fn, int root, std::optional<Ref> self, bool callbackStart,
                    bool rootStart);
  int newRoot(int parent, unsigned fn, std::optional<Ref> self, unsigned callerFn, unsigned rec,
              Mode mode);
  bool scheduled(const Activation &act, unsigned seg) const;
  void findPieces();
  void findHosts();
  std::vector<unsigned> piecesAt(const Activation &act, llvm::ArrayRef<unsigned> segs) const;
  bool once(unsigned fi, unsigned rec, unsigned rootFn, unsigned depth);
  bool fnOnce(unsigned fi, unsigned rootFn, unsigned depth);
  bool rejectionEnds(const AwaitExpr *a, unsigned depth);
  bool fnRejectionEnds(unsigned fi, unsigned depth);
  void findJoins();
  void buildFlow();
  void findBindings();
  bool keeps(const PieceInst &piece, const Access &a) const;
  std::vector<unsigned> reach(int root, const std::vector<unsigned> &from) const;
  std::vector<unsigned> reachAfter(int root,
                                   const std::vector<std::pair<unsigned, llvm::SMLoc>> &from);

private:
  bool ownPiece(unsigned act, unsigned piece) const;
  int stepOf(unsigned fn, llvm::SMLoc loc);
  bool later(unsigned fn, llvm::SMLoc at, llvm::SMLoc origin);
  void addEntry(unsigned act, unsigned piece, const Analysis::Entry &add, bool &changed);
  void noteFlow(int root, unsigned from, unsigned to, llvm::SMLoc origin);
  Analysis &analysis_;
};

}
