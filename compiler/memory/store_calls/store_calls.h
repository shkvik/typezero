#pragma once

#include "compiler/memory/store_scanner/store_scanner.h"

namespace tz {

// The stores calls make, of functions and of built-ins.
class StoreCalls {
public:
  explicit StoreCalls(StoreScanner &scanner);

  StoreScanner::Sources call(const CallExpr *c);
  // The built-ins that keep none of their operands: the others may.
  StoreScanner::Sources builtin(const CallExpr *c, const StoreScanner::Sources &receiver,
                                const std::vector<StoreScanner::Sources> &args);

private:
  StoreScanner &scanner_;
};

} // namespace tz
