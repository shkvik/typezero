#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Where the literals of a function go, found before its body is checked.
class DestinationScanner {
public:
  explicit DestinationScanner(Sema &sema);

  void scanDestinations(const Stmt *stmt, Sema::DestinationScan &scan);
  // Returns false once a declaration hides the variable for the rest.
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, Sema::DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, Sema::DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, Sema::DestinationScan &scan);
  // The type of a simple expression (a variable, its fields and elements)
  // without checking it, or null.
  const Type *peekType(const Expr *expr, const Sema::DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const Sema::DestinationScan &scan) const;
  // What the arguments of a call go to, by position.
  std::vector<const Type *> calleeParams(const CallExpr *call, const Sema::DestinationScan &scan);

private:
  Sema &sema_;
};

} // namespace tz
