#pragma once

#include "compiler/memory/store_scanner/store_scanner.h"

namespace tz {

// The values expressions give, and the stores they make.
class StoreExprs {
public:
  explicit StoreExprs(StoreScanner &scanner);

  // The places the value of `e` may come from.
  StoreScanner::Sources expr(const Expr *e);

private:
  StoreScanner &scanner_;
};

} // namespace tz
