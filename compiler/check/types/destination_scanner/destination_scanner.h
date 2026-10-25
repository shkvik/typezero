#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Where the literals of a function go, found before its body is checked.
class DestinationScanner {
public:
  explicit DestinationScanner(Checker &checker);

  void scanDestinations(const Stmt *stmt, Checker::DestinationScan &scan);
  // Returns false once a declaration hides the variable for the rest.
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, Checker::DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, Checker::DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, Checker::DestinationScan &scan);
  // The type of a simple expression (a variable, its fields and elements)
  // without checking it, or null.
  const Type *peekType(const Expr *expr, const Checker::DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const Checker::DestinationScan &scan) const;
  // What the arguments of a call go to, by position.
  std::vector<const Type *> calleeParams(const CallExpr *call,
                                         const Checker::DestinationScan &scan);

private:
  Checker &checker_;
};

} // namespace tz
