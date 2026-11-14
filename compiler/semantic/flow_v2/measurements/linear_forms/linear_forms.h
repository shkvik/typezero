#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow_v2/measurements/builtin_lengths/builtin_lengths.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include <optional>
#include <vector>

namespace tz {

class SemanticState;

enum class ProofAsk { LINEAR, LENGTH };

struct ProofNode {
  ProofAsk ask;
  const Expr *expr;
};

class LinearForms {
public:
  LinearForms(SemanticState &owner, BuiltinLengths &sharedBuiltinLengths);

  bool isRawBuffer(const Type *type) const;


  std::optional<Linear> linear(const Expr *expr) const;
  std::optional<Linear> lengthOf(const Expr *expr) const;
  std::optional<Linear> knownLength(const Expr *value) const;
  std::optional<Linear> lengthAtMost(const Expr *expr) const;


  Linear settled(Linear value) const;
  IntegerRange lengthRange(const Expr *value) const;
  const VarSymbol *variableOf(const Expr *expr) const;
  std::optional<int64_t> exactLength(const Expr *object) const;

private:
  bool isFittingInt(const Linear &sum) const;
  bool isLinearStep(const BinaryExpr *binary) const;
  bool isTwoArgumentPart(const CallExpr *call) const;
  bool isIntWidening(const CastExpr *conversion) const;
  bool hasSpreadElement(const ArrayLitExpr *list) const;


  const Expr *viewedSource(const Expr *value) const;
  const Expr *measuredObject(const Expr *expr) const;
  const Expr *measuredMember(const MemberExpr *member) const;


  const CallExpr *innerView(const Expr *value) const;
  const CallExpr *wholeByteView(const Expr *value) const;


  std::optional<Linear> proofAnswer(ProofNode root) const;
  std::optional<Linear> baseLength(const Expr *value) const;
  std::optional<Linear> viewStart(const CallExpr *call) const;
  std::optional<Linear> nameLinear(const NameExpr *name) const;
  std::optional<Linear> callBaseLength(const CallExpr *call) const;
  std::optional<Linear> allocatedLength(const CallExpr *call) const;
  std::optional<Linear> negatedLinear(const UnaryExpr *unary) const;
  std::optional<Linear> constructedLength(const NewExpr *create) const;
  std::optional<Linear> literalLinear(const IntLitExpr *literal) const;
  std::optional<Linear> variableLength(const VarSymbol *variable) const;
  std::optional<Linear> listLiteralLength(const ArrayLitExpr *list) const;
  std::optional<Linear> nodeLength(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> nodeLinear(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> viewOver(const CallExpr *call, std::optional<Linear> whole) const;
  std::optional<Linear> measuredLinear(const Expr *expr, std::vector<Linear> &answers) const;
  std::optional<Linear> nodeAnswer(const ProofNode &node, std::vector<Linear> &answers) const;
  std::optional<Linear> stepLinear(const BinaryExpr *binary, std::vector<Linear> &answers) const;
  std::optional<Linear> wideningLinear(const CastExpr *conversion,
                                       std::vector<Linear> &answers) const;
  std::optional<Linear> termSum(const BinaryExpr *binary, const Linear &left,
                                const Linear &right) const;
  std::optional<Linear> linearSum(const BinaryExpr *binary, const Linear &left,
                                  const Linear &right) const;


  NodeChildren<ProofNode> proofParts(ProofNode node) const;
  NodeChildren<ProofNode> lengthParts(const Expr *expr) const;
  NodeChildren<ProofNode> linearParts(const Expr *expr) const;


  std::optional<int64_t> exactFrom(std::optional<Linear> length) const;

  SemanticState &state;
  BuiltinLengths &builtinLengths;
};

}
