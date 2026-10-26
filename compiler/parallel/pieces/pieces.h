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
  std::vector<unsigned> reach(int root, const std::vector<unsigned> &from) const;

private:
  Analysis &analysis_;
};

}
