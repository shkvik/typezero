#pragma once

#include "compiler/memory/store_scanner/store_scanner.h"

namespace tz {

class StoreExprs {
public:
  explicit StoreExprs(StoreScanner &scanner);

  StoreScanner::Sources expr(const Expr *e);

private:
  StoreScanner &scanner_;
};

}
