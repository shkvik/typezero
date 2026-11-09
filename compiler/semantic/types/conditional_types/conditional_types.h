#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/conditional_types/conditional_types_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class TypeParams;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class TypeOperators;
class ReturnInferrer;
class ExtendsRelation;
class TopLevelChecker;

enum class FitsStepKind { GOAL, CHOICE };

struct FitsGoal {
  static constexpr FitsStepKind kind = FitsStepKind::GOAL;
  const Type *from;
  const Type *to;
  unsigned depth;
};

struct FitsChoice {
  static constexpr FitsStepKind kind = FitsStepKind::CHOICE;
  const Type *from;
  unsigned depth;
  std::vector<const Type *> branches;
  size_t next;
};

using FitsStep = Union<FitsGoal, FitsChoice>;

struct PreparedConditional {
  const Type *check = nullptr;
  const Type *pattern = nullptr;
  std::vector<const TypeRef *> inferRefs;
  std::vector<const Type *> infers;
};

struct SpreadRef {
  const TypeRef *ref;
  bool spread;
};

class ConditionalTypes {
public:
  static bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed);

  using ResultIntersectionJoined = Result<const Type *, ConditionalTypesResolveIntersectionError>;

  ConditionalTypes(SemanticState &owner, SemanticParts &all);

  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  ResultIntersectionJoined resolveIntersection(const TypeRef &ref);
  const Type *resolveTypeArgument(const TypeRef &ref);
  const Type *signatureForUtility(const TypeRef &ref);

private:
  static std::vector<const Type *> distributed(const Type *type);
  static void collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names);
  static void collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                            std::vector<bool> &lists, bool spread = false);
  static void noteInfer(SpreadRef part, std::vector<const TypeRef *> &infers,
                        std::vector<bool> &lists);
  static void pushSpreadArgs(const TypeRef &ref, std::vector<SpreadRef> &stack);
  static bool spreadsAt(const TypeRef &ref, size_t i);
  static bool mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                                  llvm::SmallPtrSetImpl<const Type *> &seen);
  static void collectParams(const Type *type, std::vector<const Type *> &params,
                            llvm::SmallPtrSetImpl<const Type *> &seen);
  static std::vector<const Type *> otherParamParts(const Type *type,
                                                   llvm::SmallPtrSetImpl<const Type *> &seen);
  static std::vector<const Type *> objectParamParts(const Type *type,
                                                    llvm::SmallPtrSetImpl<const Type *> &seen);
  static std::vector<const Type *> paramParts(const Type *type);
  static bool isBinding(const Stmt *decl);
  const Type *distributeConditional(const TypeRef &ref, llvm::ArrayRef<const Type *> members);
  const Type *memberConditional(const TypeRef &ref);
  const Type *prepareConditional(const TypeRef &ref, PreparedConditional &prepared);
  const Type *chooseBranch(const TypeRef &ref, const PreparedConditional &prepared, bool tail);
  bool fitsStep(const FitsGoal &goal, std::vector<FitsStep> &steps);
  bool retryFits(std::vector<FitsStep> &steps);
  bool tryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps);
  bool extendsWithParams(const Type *from, const Type *branch);
  SemanticState &state;
  TypeParams *typeParams = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeOperators *typeOperators = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
};

}
