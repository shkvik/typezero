#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class Scopes;
class LiteralTypes;
class TypeResolver;
class TypeRelations;
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

  bool isLiteralShaped(const Expr *expr) const;
  const Type *annotatedType(const VarDeclarator &declarator);
  const Type *declaredFunctionType(const FunctionDecl *functionDecl);
  const Type *literalDestination(const VarDeclarator &declarator,
                                 llvm::ArrayRef<VarDeclarator> later);

private:
  using DestinationPlace = std::pair<const Type *, llvm::SMLoc>;
  using LiteralNeeds = std::optional<std::vector<LiteralGoal>>;
  using WrittenNames = std::optional<std::vector<llvm::StringRef>>;

  const Type *knownReturnType() const;
  bool isLiteralShape(const Expr *expr) const;
  const Type *writable(const Type *type) const;
  bool hasReadonlyField(const Type *object) const;
  const Type *writableTuple(const Type *tuple) const;
  const VarSymbol *namedVariable(const Expr *expr) const;
  WrittenNames writtenNames(const ObjectLitExpr *literal) const;
  bool isLiteralFitting(const Expr *expr, const Type *type) const;
  std::vector<const Expr *> shapeChoices(const Expr *expr) const;
  bool isAllResults(llvm::ArrayRef<DestinationPlace> places) const;
  std::vector<const Expr *> findReturnValues(const Stmt *body) const;
  std::vector<const Expr *> mappedReturns(const CallExpr *call) const;
  LiteralNeeds unionNeeds(const Expr *expr, const Type *type) const;
  LiteralNeeds callNeeds(const CallExpr *call, const Type *type) const;
  const Type *unionMemberFor(const Expr *expr, const Type *type) const;
  LiteralNeeds literalNeeds(const Expr *expr, const Type *wanted) const;
  LiteralNeeds resultNeeds(const CallExpr *call, const Type *type) const;
  const Type *commonPlace(llvm::ArrayRef<DestinationPlace> places) const;
  WrittenNames spreadNames(const ObjectLitExpr::Property &property) const;
  const Type *unifiedResults(llvm::ArrayRef<DestinationPlace> places) const;
  WrittenNames propertyNames(const ObjectLitExpr::Property &property) const;
  const Type *chooseDestination(llvm::ArrayRef<DestinationPlace> places) const;
  LiteralNeeds arrayNeeds(const ArrayLitExpr *literal, const Type *type) const;
  LiteralNeeds tupleNeeds(const ArrayLitExpr *literal, const Type *tuple) const;
  LiteralNeeds objectNeeds(const ObjectLitExpr *literal, const Type *type) const;
  void scanFollowing(llvm::StringRef name, bool isVisibleAtStart, DestinationScan &scan);
  bool isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls, llvm::StringRef name) const;
  void scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later, DestinationScan &scan);
  bool isCoveringFields(const Type *type, llvm::ArrayRef<llvm::StringRef> names) const;
  bool isAcceptingAll(const Type *candidate, llvm::ArrayRef<DestinationPlace> places) const;
  std::vector<DestinationPlace> fittingPlaces(const Expr *init, const DestinationScan &scan);
  std::vector<LiteralGoal> conditionalNeeds(const ConditionalExpr *conditional,
                                            const Type *type) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  IterableChecker *iterableChecker = nullptr;
  DestinationScanner *destinationScanner = nullptr;
};

}
