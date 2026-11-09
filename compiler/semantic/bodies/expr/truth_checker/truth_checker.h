#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class JsonChecker;
class TypeRelations;
class ConditionFacts;
class GenericChecker;

enum class BranchStepKind { EXPR, LEFT_DONE, RIGHT_DONE, NOT_DONE };

struct BranchExpr {
  static constexpr BranchStepKind kind = BranchStepKind::EXPR;
  Expr *expr;
  TruthUse use;
};

struct LeftDone {
  static constexpr BranchStepKind kind = BranchStepKind::LEFT_DONE;
  BinaryExpr *bin;
};

struct RightDone {
  static constexpr BranchStepKind kind = BranchStepKind::RIGHT_DONE;
  BinaryExpr *bin;
  Flow lhsTrue;
  Flow lhsFalse;
};

struct NotDone {
  static constexpr BranchStepKind kind = BranchStepKind::NOT_DONE;
  UnaryExpr *unary;
};

using BranchStep = Union<BranchExpr, LeftDone, RightDone, NotDone>;

struct BranchResult {
  Expr *expr = nullptr;
  Flow whenTrue;
  Flow whenFalse;
};

struct BranchRun {
  std::vector<BranchStep> steps;
  std::vector<BranchResult> results;
};

class TruthChecker {
public:
  TruthChecker(SemanticState &owner, SemanticParts &all);

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
  static void notDone(UnaryExpr *unary, BranchRun &run);
  void applyBranchStep(BranchStep &step, BranchRun &run);
  void enterBranch(const BranchExpr &branch, BranchRun &run);
  void leftDone(BinaryExpr *bin, BranchRun &run);
  void rightDone(RightDone &done, BranchRun &run);
  Expr *branchLeaf(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use);
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
  static NodeChildren<const Expr *> sourceParts(const Expr *expr);
  static std::string nodeSource(const Expr *expr, std::vector<std::string> &answers);
  static std::string stringSource(const StringLitExpr *lit);
  static std::string callSource(const CallExpr *call, std::vector<std::string> &answers);
  static std::string hintText(const Expr *expr, const char *placeholder = "x");
  static const NameExpr *narrowedVariable(const Expr *expr);
  static bool isLength(const Expr *expr);
  static bool isLiteral(const Expr *expr);
  static const Type *falsyType(const Type *type);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
