#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/truth_checker/truth_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TruthChecker {
public:
  explicit TruthChecker(CheckState &owner);

  Expr *checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse,
                       TruthUse use = TruthUse::Condition);
  Expr *checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue, Flow &whenFalse,
                         TruthUse use);
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Flow &whenTrue, Flow &whenFalse,
                     bool tested, TruthUse use);
  Expr *checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  using ResultTruthIsTested = Result<const Expr *, TruthCheckerCheckTruthyError>;
  using ResultLogicalJoined = Result<const Type *, TruthCheckerLogicalValueError>;
  using ResultConditionForm = Result<const AssignExpr *, TruthCheckerAssignmentConditionError>;

  static ResultLogicalJoined logicalValue(const BinaryExpr *bin, llvm::StringRef op, bool isAnd,
                                          const Type *kept);
  static ResultConditionForm assignmentCondition(const AssignExpr *assign, TruthUse use);
  ResultTruthIsTested checkTruthy(const Expr *expr, TruthUse use, const Expr *other = nullptr);
  static llvm::StringRef useOperator(TruthUse use);
  static std::string paramMessage(const Type *type, const Type *base, TruthUse use,
                                  llvm::StringRef x, llvm::StringRef y);
  static std::string jsonMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string unknownMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string numberMessage(const Expr *expr, const Type *type, const Type *base,
                                   TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string objectMessage(const Expr *expr, const Type *base, TruthUse use,
                                   llvm::StringRef x, llvm::StringRef y);
  static std::string sourceText(const Expr *expr);
  static std::string hintText(const Expr *expr, const char *placeholder = "x");
  static const NameExpr *narrowedVariable(const Expr *expr);
  static bool isLength(const Expr *expr);
  static bool isLiteral(const Expr *expr);
  static const Type *falsyType(const Type *type);

  CheckState &state;
};

}
