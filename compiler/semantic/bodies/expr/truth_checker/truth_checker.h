#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class FlowChecker;
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
  BinaryExpr *binary;
};

struct RightDone {
  static constexpr BranchStepKind kind = BranchStepKind::RIGHT_DONE;
  BinaryExpr *binary;
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

enum class RightSide {
  TESTED,
  VALUE,
};

class TruthChecker {
public:
  TruthChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse,
                       TruthUse use = TruthUse::Condition);
  Expr *checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue, Flow &whenFalse,
                         TruthUse use);
  Expr *checkLogical(BinaryExpr *binary, const Type *expected, Flow &whenTrue, Flow &whenFalse,
                     RightSide side, TruthUse use);
  Expr *checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  using ResultTruthIsTested = Result<const Expr *, TruthCheckerCheckTruthyError>;
  using ResultLogicalJoined = Result<const Type *, TruthCheckerLogicalValueError>;
  using ResultConditionForm = Result<const AssignExpr *, TruthCheckerAssignmentConditionError>;

  struct ObjectTruth {
    const NameExpr *narrowed = nullptr;
    bool isArray = false;
    bool isPromise = false;
    bool isClass = false;
    bool isObject = false;
    bool isClassName = false;
  };

  std::string sourceText(const Expr *expr) const;
  NodeChildren<const Expr *> sourceParts(const Expr *expr) const;
  std::string nodeSource(const Expr *expr, std::vector<std::string> &answers) const;
  std::string memberSource(const MemberExpr *member, std::vector<std::string> &answers) const;
  std::string indexSource(const IndexExpr *index, std::vector<std::string> &answers) const;
  std::string stringSource(const StringLitExpr *literal) const;
  std::string callSource(const CallExpr *call, std::vector<std::string> &answers) const;
  std::string hintText(const Expr *expr, const char *placeholder = "x") const;
  const NameExpr *narrowedVariable(const Expr *expr) const;
  bool isLength(const Expr *expr) const;
  const Expr *withoutWrappers(const Expr *expr) const;
  const Expr *wrappedExpr(const Expr *expr) const;
  bool isLiteral(const Expr *expr) const;
  const Type *falsyType(const Type *type) const;
  void applyBranchStep(BranchStep &step, BranchRun &run);
  void enterBranch(const BranchExpr &branch, BranchRun &run);
  void leftDone(BinaryExpr *binary, BranchRun &run);
  void rightDone(RightDone &done, BranchRun &run);
  void notDone(UnaryExpr *unary, BranchRun &run) const;
  Expr *branchLeaf(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use);
  Expr *assignedLeaf(AssignExpr *assign, Flow &whenTrue, Flow &whenFalse, TruthUse use);
  void addValueFacts(Expr *value, Flow &whenTrue, Flow &whenFalse);
  bool walkTestedRight(BinaryExpr *binary, const Type *hint, Flow &rhsTrue, Flow &rhsFalse);
  Expr *checkLogicalType(BinaryExpr *binary, const Type *leftType, bool isTruthy, TruthUse use);
  std::string logicalSpelling(BinaryOp op, bool isAssign) const;
  const Type *keptLeftType(const Type *leftType, bool isAnd) const;
  const Type *failedLogicalType(const Type *leftType, const Type *rightType,
                                const Type *kept) const;
  llvm::StringRef useOperator(TruthUse use) const;
  std::string paramMessage(const Type *type, const Type *base, TruthUse use,
                           llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string jsonMessage(TruthUse use, llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string unknownMessage(TruthUse use, llvm::StringRef valueText,
                             llvm::StringRef otherText) const;
  std::string numberMessage(const Expr *expr, const Type *type, const Type *base, TruthUse use,
                            llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string optionalNumberAdvice(TruthUse use, bool isLength, llvm::StringRef valueText,
                                   llvm::StringRef otherText) const;
  std::string objectMessage(const Expr *expr, const Type *base, TruthUse use,
                            llvm::StringRef valueText, llvm::StringRef otherText) const;
  ObjectTruth objectTruth(const Expr *expr, const Type *base) const;
  std::string objectHead(const ObjectTruth &truth, TruthUse use) const;
  std::string objectSubject(const ObjectTruth &truth, llvm::StringRef what) const;
  std::string useConsequence(TruthUse use) const;
  std::string objectAdvice(const ObjectTruth &truth, const Type *base, TruthUse use,
                           llvm::StringRef valueText, llvm::StringRef otherText) const;
  ResultTruthIsTested checkTruthy(const Expr *expr, TruthUse use, const Expr *other = nullptr);
  std::optional<TruthCheckerCheckTruthyErrorCode> truthProblem(const Type *type,
                                                               const Type *base) const;
  bool isTestable(const Type *type, const Type *base) const;
  bool areAllBoolOrString(llvm::ArrayRef<const Type *> types) const;
  std::optional<TruthCheckerCheckTruthyErrorCode> unionProblem(const Type *type,
                                                               const Type *base) const;
  std::vector<const Type *> unionMembers(const Type *base) const;
  const Type *unionNumber(const Type *base) const;
  bool isNumberMember(const Type *member) const;
  bool hasFalsyMember(const Type *base) const;
  bool isFalsyMember(const Type *member) const;
  std::string truthMessage(TruthCheckerCheckTruthyErrorCode code, const Expr *expr, TruthUse use,
                           const Expr *other, const Type *base) const;
  std::string unionNumberMessage(const Type *type, const Type *base, llvm::StringRef value) const;
  ResultConditionForm checkAssignmentCondition(const AssignExpr *assign, TruthUse use) const;
  ResultLogicalJoined checkLogicalValue(const BinaryExpr *binary, llvm::StringRef op, bool isAnd,
                                        const Type *kept) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
