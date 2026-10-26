#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DestinationScanner {
public:
  explicit DestinationScanner(Checker &checker);

  void scanDestinations(const Stmt *stmt, Checker::DestinationScan &scan);
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, Checker::DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, Checker::DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, Checker::DestinationScan &scan);
  const Type *peekType(const Expr *expr, const Checker::DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const Checker::DestinationScan &scan) const;
  std::vector<const Type *> calleeParams(const CallExpr *call,
                                         const Checker::DestinationScan &scan);

private:
  Checker &checker_;
};

}
