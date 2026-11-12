#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

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

enum class ArgumentStrength {
  STRONG,
  WEAK,
};

class Inference {
public:
  using TypeOf = std::function<const Type *(const Type *)>;

  Inference(llvm::ArrayRef<const Type *> typeParams, const AliasStore &aliases,
            TypeOf elementOfType);

  bool hasDeferred() const;
  void setAsyncOf(TypeOf asyncOfType);
  void forgetArgumentMarks();
  bool hasSeenUndefined(size_t index) const;
  void from(const Type *pattern, const Type *actual);
  void retry(const std::vector<const Type *> &bound);
  void fromHeld(const Type *pattern, const Type *actual);
  void adoptCandidates(size_t index, const Inference &other);
  const std::vector<const Type *> &getCandidates(size_t index) const;
  const std::vector<const Type *> &getWeakCandidates(size_t index) const;
  const std::vector<const Type *> &getLiteralCandidates(size_t index) const;
  void markArgument(ArgumentStrength strength, bool isIntLiteralArgument);
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);

private:
  struct ListLengths {
    std::vector<int> sizes;
    size_t known;
    int unknown;
    bool isAmbiguous;
  };

  static bool isFixedList(const Type *value);
  static bool isInstanceOfMember(const Type *value, const Type *member);
  static std::vector<InferStep> recordFields(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferStepList(const InferList &list);
  std::vector<InferStep> addCandidate(const AddCandidate &candidate);
  const Type *boundValue(const Type *param, const std::vector<const Type *> *bound) const;
  ListLengths listLengths(const Type *pattern, const std::vector<const Type *> *bound) const;
  bool fitListLengths(ListLengths &lengths, const Type *pattern, const Type *actual) const;
  void addListLength(const Type *part, size_t index, const std::vector<const Type *> *bound,
                     ListLengths &lengths) const;
  std::vector<InferStep> listPairs(const Type *pattern, const Type *actual,
                                   llvm::ArrayRef<int> sizes) const;
  static const Type *paramsAsList(const Type *functionType, size_t from);
  static const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);

  void runInference(InferStep first);
  std::vector<InferStep> applyInferStep(const InferStep &step);
  std::vector<InferStep> inferPair(const Type *pattern, const Type *actual);
  std::optional<size_t> paramIndex(const Type *type) const;
  std::vector<InferStep> inferParam(size_t index, const Type *pattern, const Type *actual);
  std::vector<const Type *> &candidatesFor(size_t index);
  std::vector<InferStep> inferInside(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferFunction(const Type *pattern, const Type *actual);
  static std::vector<InferStep> iteratorParts(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferDict(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferConstructor(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferObject(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferAsyncIterator(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferClassArgs(const Type *pattern, const ClassDecl *classDecl,
                                               const Type *actual);
  std::optional<std::vector<InferStep>> inferAliasArgs(const AliasOrigin *origin,
                                                       const Type *actual);
  static std::vector<InferStep> pairArgs(llvm::ArrayRef<const Type *> want,
                                         llvm::ArrayRef<const Type *> have);
  static std::vector<InferStep> inferFields(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferUnion(const Type *pattern, const Type *actual);
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
  const AliasStore &origins;
  TypeOf elementOf;
  TypeOf asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

}
