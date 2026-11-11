#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class Scopes;
class LiteralTypes;
class TypeResolver;
class IterableChecker;
class DestinationScan;
class DestinationScanner;

struct LiteralGoal {
  const Expr *expr;
  const Type *type;
};

class Destination {
public:
  Destination(SemanticState &owner, SemanticParts &all);

  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  static bool literalShaped(const Expr *expr);
  bool literalFits(const Expr *expr, const Type *type) const;
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  using DestinationPlace = std::pair<const Type *, llvm::SMLoc>;

  void scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later, DestinationScan &scan);
  const Type *annotatedType(const VarDeclarator &decl);
  static bool isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls, llvm::StringRef name);
  void scanFollowing(llvm::StringRef name, bool visible, DestinationScan &scan);
  std::vector<DestinationPlace> fittingPlaces(const Expr *init, const DestinationScan &scan);
  static const Type *chooseDestination(llvm::ArrayRef<DestinationPlace> places);
  static const Type *commonPlace(llvm::ArrayRef<DestinationPlace> places);
  static bool acceptsAll(const Type *candidate, llvm::ArrayRef<DestinationPlace> places);
  static bool allResults(llvm::ArrayRef<DestinationPlace> places);
  static const Type *unifiedResults(llvm::ArrayRef<DestinationPlace> places);
  static std::vector<const Expr *> findReturnValues(const Stmt *body);
  static std::optional<std::vector<LiteralGoal>> arrayNeeds(const ArrayLitExpr *lit,
                                                            const Type *type);
  static std::optional<std::vector<LiteralGoal>> callNeeds(const CallExpr *call, const Type *type);
  std::optional<std::vector<LiteralGoal>> literalNeeds(const Expr *expr, const Type *wanted) const;
  std::optional<std::vector<LiteralGoal>> unionNeeds(const Expr *expr, const Type *type) const;
  std::optional<std::vector<LiteralGoal>> objectNeeds(const ObjectLitExpr *lit,
                                                      const Type *type) const;
  static bool isLiteralShape(const Expr *expr);
  static std::vector<const Expr *> shapeChoices(const Expr *expr);
  static const Type *writable(const Type *type);
  SemanticState &state;
  Scopes *scopes = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  IterableChecker *iterableChecker = nullptr;
  DestinationScanner *destinationScanner = nullptr;
};

}
