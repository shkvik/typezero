#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/index_proofs/index_proofs_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class LibraryCalls;
class ConstantFolder;

class IndexProofs {
public:
  using ResultBetweenProven = Result<const Expr *, IndexProofsRequireBetweenError>;
  using ResultLessWasProven = Result<const Expr *, IndexProofsRequireLessError>;
  using ResultUpToWasProven = Result<const Expr *, IndexProofsRequireUpToError>;

  IndexProofs(SemanticState &owner, SemanticParts &all);

  std::optional<Linear> linear(const Expr *expr) const;
  std::optional<Linear> lengthOf(const Expr *object) const;
  std::optional<Linear> knownLength(const Expr *value) const;
  bool provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const;
  bool provesAtInBounds(const Expr *object, const Expr *index) const;
  bool provesNonNegative(const Expr *expr) const;
  bool provesNotEmpty(const Expr *object) const;
  bool provesBetween(const Expr *expr, int64_t lo, std::optional<int64_t> hi) const;
  bool exempt(llvm::SMLoc loc, const Expr *object = nullptr) const;
  ResultBetweenProven requireBetween(const Expr *expr, int64_t lo, std::optional<int64_t> hi,
                                     llvm::StringRef what);
  bool provesLess(const Expr *a, const Expr *b) const;
  ResultLessWasProven requireLess(const Expr *a, const Expr *b, llvm::StringRef what);
  bool provesLinearUpTo(Linear value, const Expr *object, int64_t less) const;
  bool provesUpTo(const Expr *value, const Expr *object, int64_t less) const;
  ResultUpToWasProven requireUpTo(const Expr *value, const Expr *object, int64_t less,
                                  llvm::StringRef what);
  std::optional<int64_t> exactLength(const Expr *object) const;
  std::optional<Linear> viewLength(const CallExpr *call) const;
  std::optional<Linear> utf8Length(const CallExpr *call) const;
  void addIndexCallbackFacts(const FunctionDecl *fn);
  std::optional<Linear> freshBufferLength(const CallExpr *call) const;
  IntegerRange lengthRange(const Expr *value) const;
  std::optional<Linear> lengthAtMost(const Expr *expr) const;
  Linear settled(Linear value) const;
  bool provesSumUpTo(std::optional<Linear> a, std::optional<Linear> b, const Expr *object) const;

private:
  static const VarSymbol *variableOf(const Expr *expr);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
