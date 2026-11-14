#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/logic/code_logic_registry_errors.h"
#include "compiler/semantic/code/logic/logic_rules/logic_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class LogicValues {
public:
  LogicValues(SemanticState &owner, LogicRules &sharedLogicRules);

  void afterTestedLeaf();
  void afterLogicalLeft(const AfterLogicalLeft &task);
  void startLogicalRight(const StartLogicalRight &task);
  void finishLogicalType(const FinishLogicalType &task);
  void finishValueLogical(const FinishValueLogical &task);
  void finishTestedLogical(const FinishTestedLogical &task);
  void checkLogicalOperator(const CheckLogicalOperator &task);

private:
  using ResultLogicalJoined = Result<const Type *, CodeLogicRegistryLogicalValueError>;

  void joinLogicalFlows(LogicalDraft draft, ConditionFlows right);


  const Type *falsyType(const Type *type) const;
  const Type *keptLeftType(const Type *leftType, bool isAnd) const;
  const Type *failedLogicalType(const Type *leftType, const Type *rightType,
                                const Type *kept) const;


  std::string logicalSpelling(BinaryOp op, bool isAssign) const;
  ResultLogicalJoined checkLogicalValue(const BinaryExpr *binary, llvm::StringRef op, bool isAnd,
                                        const Type *kept) const;

  SemanticState &state;
  LogicRules &logicRules;
};

}
