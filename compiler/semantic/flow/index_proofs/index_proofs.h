#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/index_proofs/index_proofs_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class AwaitExpr;
class ByteProofs;
class LibraryCalls;
class ConstantFolder;
class GenericChecker;

enum class ProofAsk { LINEAR, LENGTH };

struct ProofNode {
  ProofAsk ask;
  const Expr *expr;
};

class IndexProofs {
public:
  using ResultBetweenProven = Result<const Expr *, IndexProofsRequireBetweenError>;
  using ResultLessWasProven = Result<const Expr *, IndexProofsRequireLessError>;
  using ResultUpToWasProven = Result<const Expr *, IndexProofsRequireUpToError>;

  IndexProofs(SemanticState &owner, SemanticParts &all);

  std::optional<Linear> linear(const Expr *expr) const;
  std::optional<Linear> lengthOf(const Expr *expr) const;
  std::optional<Linear> knownLength(const Expr *value) const;
  bool canProveInBounds(const Expr *object, const Expr *index) const;
  bool canProveInBoundsOrEnd(const Expr *object, const Expr *index) const;
  bool canProveAtInBounds(const Expr *object, const Expr *index) const;
  bool canProveNonNegative(const Expr *expr) const;
  bool canProveNotEmpty(const Expr *object) const;
  bool canProveBetween(const Expr *expr, int64_t low, std::optional<int64_t> high) const;
  bool isExempt(llvm::SMLoc loc, const Expr *object = nullptr) const;
  ResultBetweenProven requireBetween(const Expr *expr, int64_t low, std::optional<int64_t> high,
                                     llvm::StringRef what);
  bool canProveLess(const Expr *left, const Expr *right) const;
  ResultLessWasProven requireLess(const Expr *left, const Expr *right, llvm::StringRef what);
  bool canProveLinearUpTo(Linear value, const Expr *object, int64_t less) const;
  bool canProveUpTo(const Expr *value, const Expr *object, int64_t less) const;
  ResultUpToWasProven requireUpTo(const Expr *value, const Expr *object, int64_t less,
                                  llvm::StringRef what);
  std::optional<int64_t> exactLength(const Expr *object) const;
  std::optional<Linear> utf8Length(const CallExpr *call) const;
  void addIndexCallbackFacts(const FunctionDecl *functionDecl);
  IntegerRange lengthRange(const Expr *value) const;
  std::optional<Linear> lengthAtMost(const Expr *expr) const;
  Linear settled(Linear value) const;
  bool canProveSumUpTo(std::optional<Linear> first, std::optional<Linear> second,
                       const Expr *object) const;

private:
  const VarSymbol *variableOf(const Expr *expr) const;
  std::optional<Linear> proofAnswer(ProofNode root) const;
  NodeChildren<ProofNode> proofParts(ProofNode node) const;
  std::optional<Linear> nodeAnswer(const ProofNode &node, std::vector<Linear> &answers) const;
  NodeChildren<ProofNode> linearParts(const Expr *expr) const;
  NodeChildren<ProofNode> lengthParts(const Expr *expr) const;
  bool isSizedAllocation(const CallExpr *call) const;
  bool isRawBuffer(const Type *type) const;
  const Expr *measuredObject(const Expr *expr) const;
  const Expr *measuredMember(const MemberExpr *member) const;
  const Expr *measuredCall(const CallExpr *call) const;
  bool isIntWidening(const CastExpr *conversion) const;
  bool isLinearStep(const BinaryExpr *binary) const;
  std::optional<Linear> nodeLinear(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> literalLinear(const IntLitExpr *literal) const;
  std::optional<Linear> negatedLinear(const UnaryExpr *unary) const;
  std::optional<Linear> nameLinear(const NameExpr *name) const;
  std::optional<Linear> measuredLinear(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> wideningLinear(const CastExpr *conversion,
                                       std::vector<Linear> &answers) const;
  std::optional<Linear> stepLinear(const BinaryExpr *binary, std::vector<Linear> &answers) const;
  std::optional<Linear> linearSum(const BinaryExpr *binary, const Linear &left,
                                  const Linear &right) const;
  std::optional<Linear> termSum(const BinaryExpr *binary, const Linear &left,
                                const Linear &right) const;
  bool isFittingInt(const Linear &sum) const;
  std::optional<Linear> nodeLength(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> listLiteralLength(const ArrayLitExpr *list) const;
  bool hasSpreadElement(const ArrayLitExpr *list) const;
  std::optional<Linear> variableLength(const VarSymbol *variable) const;
  std::optional<Linear> callLength(const CallExpr *call, std::vector<Linear> &answers) const;
  std::optional<Linear> embeddedLength(const CallExpr *call) const;
  std::optional<Linear> allocatedCount(std::vector<Linear> &answers) const;
  std::optional<Linear> freshBufferLength(const CallExpr *call, std::vector<Linear> &answers) const;
  const CallExpr *freshBufferSource(const CallExpr *call) const;
  bool isFreshTypedArray(const CallExpr *made) const;
  int64_t utf8Size(const std::u16string &text) const;
  bool isSurrogatePair(const std::u16string &text, size_t index) const;
  int64_t utf8UnitSize(char16_t unit) const;
  bool canProveIndexWithin(const Expr *object, const Expr *index, int64_t endExtra) const;
  bool canProveLinearIndex(const Linear &at, const Linear &length, int64_t endExtra) const;
  std::vector<const Expr *> provedBranches(const Expr *expr) const;
  const Expr *withoutWidening(const Expr *expr) const;
  bool isValueWidening(const CastExpr *conversion) const;
  bool canProveValueBetween(const Expr *expr, int64_t low, std::optional<int64_t> high) const;
  bool isLinearAtLeast(const std::optional<Linear> &value, int64_t low) const;
  bool isLinearAtMost(const std::optional<Linear> &value, int64_t high) const;
  bool isNeverObject(const Expr *object) const;
  bool isNeverType(const Type *type) const;
  std::optional<Linear> lengthOrBufferLength(const Expr *object) const;
  std::optional<Linear> viewBufferLength(const Expr *object) const;
  const MemberExpr *viewBufferMember(const Expr *object) const;
  std::optional<std::string> shownValue(const Expr *value) const;
  std::optional<int64_t> exactFrom(std::optional<Linear> length) const;
  std::optional<Linear> viewOver(const CallExpr *call, std::optional<Linear> whole) const;
  std::optional<Linear> viewStart(const CallExpr *call) const;
  bool isTwoArgumentPart(const CallExpr *call) const;
  std::optional<Term> sumTerm(const Term &first, const Term &second) const;
  void addElementFacts(const FunctionDecl *functionDecl);
  void addIndexParamFacts(const FunctionDecl *functionDecl);
  const VarSymbol *paramVariable(const FunctionDecl *functionDecl, size_t index) const;
  const Expr *sameLengthSource(const Expr *object) const;
  const Expr *sameLengthInner(const Expr *object) const;
  const Expr *singleAllArgument(const AwaitExpr *awaited) const;
  const Expr *copiedSource(const CallExpr *call) const;
  const Expr *viewedSource(const Expr *value) const;
  const CallExpr *wholeByteView(const Expr *value) const;
  const CallExpr *innerView(const Expr *value) const;
  std::optional<Linear> baseLength(const Expr *value) const;
  std::optional<Linear> constructedLength(const NewExpr *create) const;
  std::optional<Linear> callBaseLength(const CallExpr *call) const;
  std::optional<Linear> allocatedLength(const CallExpr *call) const;

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
