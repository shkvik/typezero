#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class CodeOperatorsRegistry {
public:
  explicit CodeOperatorsRegistry(SemanticState &owner);

  void checkUnary(const CheckUnary &task);
  void finishVoid(const FinishVoid &task);
  void checkBinary(const CheckBinary &task);
  void finishTypeOf(const FinishTypeOf &task);
  void finishNonNull(const FinishNonNull &task);

private:
  using ResultResultNotDropped = Result<const Type *, CodeOperatorsRegistryResultKeptError>;
  using ResultNonNullAssertion = Result<const Type *, CodeOperatorsRegistryNonNullOperandError>;

  std::string indexText(std::vector<std::string> &answers) const;
  std::string memberText(const MemberExpr *member, std::vector<std::string> &answers) const;


  std::optional<std::string> simpleText(const Expr *expr) const;
  std::optional<std::string> assertedText(const Expr *expr) const;
  std::optional<std::string> nodeText(const Expr *expr, std::vector<std::string> &answers) const;


  NodeChildren<const Expr *> textParts(const Expr *expr) const;
  ResultNonNullAssertion checkNonNullOperand(const Type *type, llvm::StringRef text) const;
  ResultResultNotDropped checkResultKept(const UnaryExpr *unary, const Type *dropped,
                                         bool isPromised) const;

  SemanticState &state;
};

}
