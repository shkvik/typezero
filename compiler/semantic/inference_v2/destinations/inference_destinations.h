#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/inference_v2/destinations/destination_search/destination_search.h"
#include "compiler/semantic/inference_v2/destinations/literal_members/literal_members.h"
#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task.h"
#include "llvm/ADT/DenseMap.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

class SemanticState;
class DestinationLookahead;

using DestinationPlaces = std::vector<std::pair<const Type *, llvm::SMLoc>>;

struct DestinationFound {
  const Type *hint;
  DestinationPlaces conflict;
};

class InferenceDestinations {
public:
  explicit InferenceDestinations(SemanticState &owner);

  void rememberDestinations(const VarSymbol *variable, DestinationPlaces places);


  bool isLiteralShaped(const Expr *expr) const;
  bool isResultLiteral(const CallExpr *call) const;


  DestinationPlaces getDestinations(const VarSymbol *variable) const;
  DestinationFound literalDestination(const VarDeclarator &declarator,
                                      llvm::ArrayRef<VarDeclarator> later,
                                      llvm::ArrayRef<const Stmt *> following,
                                      DestinationTypes &types);

private:
  struct ShapeGoal {
    const Expr *expr;
    const Type *type;
  };

  using DestinationPlace = std::pair<const Type *, llvm::SMLoc>;
  using ShapeNeeds = std::optional<std::vector<ShapeGoal>>;
  using WrittenNames = std::optional<std::vector<llvm::StringRef>>;

  void scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later, DestinationLookahead &lookahead,
                            DestinationSearch &search);
  void scanFollowing(llvm::ArrayRef<const Stmt *> following, llvm::StringRef name,
                     bool isVisibleAtStart, DestinationLookahead &lookahead);


  bool isLiteralShape(const Expr *expr) const;
  bool hasReadonlyField(const Type *object) const;
  bool isLiteralFitting(const Expr *expr, const Type *type) const;
  bool isAllResults(llvm::ArrayRef<DestinationPlace> places) const;
  bool isCoveringFields(const Type *type, llvm::ArrayRef<llvm::StringRef> names) const;
  bool isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls, llvm::StringRef name) const;
  bool isAcceptingAll(const Type *candidate, llvm::ArrayRef<DestinationPlace> places) const;


  ShapeNeeds unionNeeds(const Expr *expr, const Type *type) const;
  ShapeNeeds callNeeds(const CallExpr *call, const Type *type) const;
  ShapeNeeds literalNeeds(const Expr *expr, const Type *wanted) const;
  ShapeNeeds resultNeeds(const CallExpr *call, const Type *type) const;
  ShapeNeeds arrayNeeds(const ArrayLitExpr *literal, const Type *type) const;
  ShapeNeeds tupleNeeds(const ArrayLitExpr *literal, const Type *tuple) const;
  ShapeNeeds objectNeeds(const ObjectLitExpr *literal, const Type *type) const;


  WrittenNames writtenNames(const ObjectLitExpr *literal) const;
  WrittenNames spreadNames(const ObjectLitExpr::Property &property) const;
  WrittenNames propertyNames(const ObjectLitExpr::Property &property) const;


  const Type *knownReturnType() const;
  const Type *writable(const Type *type) const;
  const Type *writableTuple(const Type *tuple) const;
  const Type *unionMemberFor(const Expr *expr, const Type *type) const;
  const Type *commonPlace(llvm::ArrayRef<DestinationPlace> places) const;
  const Type *unifiedResults(llvm::ArrayRef<DestinationPlace> places) const;
  const Type *chooseDestination(llvm::ArrayRef<DestinationPlace> places) const;


  std::vector<const Expr *> shapeChoices(const Expr *expr) const;
  std::vector<const Expr *> findReturnValues(const Stmt *body) const;
  std::vector<const Expr *> mappedReturns(const CallExpr *call) const;


  const VarSymbol *namedVariable(const Expr *expr) const;
  std::vector<DestinationPlace> fittingPlaces(const Expr *init, const DestinationSearch &search);
  std::vector<ShapeGoal> conditionalNeeds(const ConditionalExpr *conditional,
                                          const Type *type) const;

  SemanticState &state;
  LiteralMembers literalMembers;
  llvm::DenseMap<const VarSymbol *, DestinationPlaces> destinations;
};

}
