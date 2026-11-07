#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

struct DestinationScan;

class DestinationScanner {
public:
  static bool isResultLiteral(const CallExpr *call);
  explicit DestinationScanner(CheckState &owner);

  void scanDestinations(const Stmt *stmt, DestinationScan &scan);
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan);
  const Type *peekType(const Expr *expr, const DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const DestinationScan &scan) const;
  std::vector<const Type *> calleeParams(const CallExpr *call, const DestinationScan &scan);

private:
  CheckState &state;
};

}
