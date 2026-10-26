#pragma once

#include "compiler/memory/store_scanner/store_scanner.h"

namespace tz {

class StoreStmts {
public:
  explicit StoreStmts(StoreScanner &scanner);

  void stmt(const Stmt *stmt);
  void bindArgs(const FunctionDecl *fn, const std::vector<StoreScanner::Sources> &args);

private:
  StoreScanner &scanner_;
};

}
