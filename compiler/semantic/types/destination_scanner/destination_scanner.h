#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class Destination;
class TypeResolver;
class IterableChecker;
class PredicateChecker;

class DestinationScan;

class DestinationScanner {
public:
  static bool isResultLiteral(const CallExpr *call);
  DestinationScanner(SemanticState &owner, SemanticParts &all);

  void scanDestinations(const Stmt *stmt, DestinationScan &scan);
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan);
  const Type *peekType(const Expr *expr, const DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const DestinationScan &scan) const;
  std::vector<const Type *> calleeParams(const CallExpr *call, const DestinationScan &scan);

private:
  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  Destination *destination = nullptr;
  TypeResolver *typeResolver = nullptr;
  IterableChecker *iterableChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
