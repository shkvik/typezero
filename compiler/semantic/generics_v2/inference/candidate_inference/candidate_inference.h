#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/v2/generics/alias_context/alias_context.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/STLExtras.h"
#include <map>
#include <set>
#include <vector>

namespace tz {

enum class InferStepKind { PAIR, HELD, LIST, CANDIDATE };

struct InferPair {
  static constexpr InferStepKind kind = InferStepKind::PAIR;
  const Type *pattern;
  const Type *actual;
};

struct InferHeld {
  static constexpr InferStepKind kind = InferStepKind::HELD;
  const Type *pattern;
  const Type *actual;
};

struct InferList {
  static constexpr InferStepKind kind = InferStepKind::LIST;
  const Type *pattern;
  const Type *actual;
  const std::vector<const Type *> *bound;
};

struct AddCandidate {
  static constexpr InferStepKind kind = InferStepKind::CANDIDATE;
  size_t index;
  const Type *type;
};

using InferStep = Union<InferPair, InferHeld, InferList, AddCandidate>;

enum class IterationLookups {
  NONE,
  ELEMENTS,
  ELEMENTS_AND_ASYNC,
};

enum class ArgumentStrength {
  STRONG,
  WEAK,
};

class CandidateInference {
public:
  CandidateInference(llvm::ArrayRef<const Type *> typeParams, const AliasContext &aliases,
                     IterationLookups iterationLookups);

  void forgetArgumentMarks();
  void copyLookups(const CandidateInference &other);
  void from(const Type *pattern, const Type *actual);
  void retry(const std::vector<const Type *> &bound);
  void fromHeld(const Type *pattern, const Type *actual);
  void rememberAsyncType(const Type *type, const Type *async);
  void rememberElementType(const Type *type, const Type *element);
  void adoptCandidates(size_t index, const CandidateInference &other);
  void markArgument(ArgumentStrength strength, bool isIntLiteralArgument);
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);


  bool hasDeferred() const;
  bool hasSeenUndefined(size_t index) const;


  const std::vector<const Type *> &getMissingAsyncTypes() const;
  const std::vector<const Type *> &getMissingElementTypes() const;
  const std::vector<const Type *> &getCandidates(size_t index) const;
  const std::vector<const Type *> &getWeakCandidates(size_t index) const;
  const std::vector<const Type *> &getLiteralCandidates(size_t index) const;

private:
  struct ListLengths {
    std::vector<int> sizes;
    size_t known;
    int unknown;
    bool isAmbiguous;
  };

  static bool isFixedList(const Type *value);
  static bool isInstanceOfMember(const Type *value, const Type *member);
  bool fitListLengths(ListLengths &lengths, const Type *pattern, const Type *actual) const;


  std::vector<InferStep> inferStepList(const InferList &list);
  std::vector<InferStep> addCandidate(const AddCandidate &candidate);
  static std::vector<InferStep> recordFields(const Type *pattern, const Type *actual);


  static const ClassDecl *instanceOf(const Type *type, const ClassDecl *generic);
  const Type *boundValue(const Type *param, const std::vector<const Type *> *bound) const;
  ListLengths listLengths(const Type *pattern, const std::vector<const Type *> *bound) const;
  void addListLength(const Type *part, size_t index, const std::vector<const Type *> *bound,
                     ListLengths &lengths) const;
  std::vector<InferStep> listPairs(const Type *pattern, const Type *actual,
                                   llvm::ArrayRef<int> sizes) const;
  void runInference(InferStep first);


  const Type *asyncOf(const Type *type);
  const Type *elementOf(const Type *type);
  static const Type *paramsAsList(const Type *functionType, size_t from);
  static const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);


  std::vector<InferStep> applyInferStep(const InferStep &step);
  std::vector<InferStep> inferPair(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferInside(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferObject(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferFunction(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferDict(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferAsyncIterator(const Type *pattern, const Type *actual);
  static std::vector<InferStep> iteratorParts(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferConstructor(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferParam(size_t index, const Type *pattern, const Type *actual);


  std::vector<const Type *> &candidatesFor(size_t index);
  std::optional<size_t> paramIndex(const Type *type) const;
  static std::vector<InferStep> inferClassArgs(const Type *pattern, const ClassDecl *classDecl,
                                               const Type *actual);
  std::optional<std::vector<InferStep>> inferAliasArgs(const AliasOrigin *origin,
                                                       const Type *actual);
  static std::vector<InferStep> pairArgs(llvm::ArrayRef<const Type *> want,
                                         llvm::ArrayRef<const Type *> have);
  std::vector<InferStep> inferUnion(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferFields(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferGenericMember(llvm::ArrayRef<const Type *> open,
                                                   const Type *actual);
  static std::vector<const Type *> membersOutside(const Type *value,
                                                  llvm::ArrayRef<const Type *> fixed);
  std::vector<InferStep> inferHeld(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferList(const Type *pattern, const Type *actual,
                                   const std::vector<const Type *> *bound);

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates, literalCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool isWeak = false;
  bool isIntLiteral = false;
  const AliasContext *origins;
  IterationLookups lookups;
  std::map<const Type *, const Type *> elementTypes, asyncTypes;
  std::vector<const Type *> missingElementTypes, missingAsyncTypes;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

}
