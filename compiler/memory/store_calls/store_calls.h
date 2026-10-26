#pragma once

#include "compiler/memory/store_scanner/store_scanner.h"

namespace tz {

class StoreCalls {
public:
  explicit StoreCalls(StoreScanner &scanner);

  StoreScanner::Sources call(const CallExpr *c);
  StoreScanner::Sources builtin(const CallExpr *c, const StoreScanner::Sources &receiver,
                                const std::vector<StoreScanner::Sources> &args);

private:
  StoreScanner &scanner_;
};

}
