#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/truth_checker/truth_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TruthChecker {
public:
  explicit TruthChecker(Checker &owner);

  Expr *checkCondition(Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Checker::Flow &whenTrue, Checker::Flow &whenFalse,
                       Checker::TruthUse use = Checker::TruthUse::Condition);
  Expr *checkTestedValue(Expr *expr, const Type *expected, Checker::Flow &whenTrue,
                         Checker::Flow &whenFalse, Checker::TruthUse use);
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Checker::Flow &whenTrue,
                     Checker::Flow &whenFalse, bool tested, Checker::TruthUse use);
  Expr *checkBooleanCall(CallExpr *call, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  using ResultTruthIsTested = Result<const Expr *, TruthCheckerCheckTruthyError>;
  using ResultLogicalJoined = Result<const Type *, TruthCheckerLogicalValueError>;
  using ResultConditionForm = Result<const AssignExpr *, TruthCheckerAssignmentConditionError>;

  static ResultLogicalJoined logicalValue(const BinaryExpr *bin, llvm::StringRef op, bool isAnd,
                                          const Type *kept);
  static ResultConditionForm assignmentCondition(const AssignExpr *assign, Checker::TruthUse use);
  ResultTruthIsTested checkTruthy(const Expr *expr, Checker::TruthUse use,
                                  const Expr *other = nullptr);
  static llvm::StringRef useOperator(Checker::TruthUse use);
  static std::string paramMessage(const Type *type, const Type *base, Checker::TruthUse use,
                                  llvm::StringRef x, llvm::StringRef y);
  static std::string jsonMessage(Checker::TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string unknownMessage(Checker::TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string numberMessage(const Expr *expr, const Type *type, const Type *base,
                                   Checker::TruthUse use, llvm::StringRef x, llvm::StringRef y);
  static std::string objectMessage(const Expr *expr, const Type *base, Checker::TruthUse use,
                                   llvm::StringRef x, llvm::StringRef y);
  static std::string sourceText(const Expr *expr);
  static std::string hintText(const Expr *expr, const char *placeholder = "x");
  static const NameExpr *narrowedVariable(const Expr *expr);
  static bool isLength(const Expr *expr);
  static bool isLiteral(const Expr *expr);
  static const Type *falsyType(const Type *type);

  Checker &checker;
};

}
