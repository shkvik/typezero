#pragma once

#include "compiler/lower/memory/store_scanner/store_scanner.h"

namespace tz {

// The stores statements make.
class StoreStmts {
public:
  explicit StoreStmts(StoreScanner &scanner);

  void stmt(const Stmt *stmt);
  // The arguments of a call of `fn` (null: of code not known) go to its
  // parameters.
  void bindArgs(const FunctionDecl *fn, const std::vector<StoreScanner::Sources> &args);

private:
  StoreScanner &scanner_;
};

} // namespace tz
