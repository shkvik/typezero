#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class Scopes;
class LiteralTypes;
class TypeResolver;
class IterableChecker;
class DestinationScanner;

class Destination {
public:
  Destination(SemanticState &owner, SemanticParts &all);

  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  static bool literalShaped(const Expr *expr);
  bool literalFits(const Expr *expr, const Type *type) const;
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  static std::vector<const Expr *> findReturnValues(const Stmt *body);
  static const Type *writable(const Type *type);
  SemanticState &state;
  Scopes *scopes = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  IterableChecker *iterableChecker = nullptr;
  DestinationScanner *destinationScanner = nullptr;
};

}
