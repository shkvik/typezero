#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/branches/code_branches_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class CodeBranchesRegistry {
public:
  explicit CodeBranchesRegistry(SemanticState &owner);

  void walkIf(const WalkIf &task);
  void finishIf(const FinishIf &task);
  void walkSwitch(const WalkSwitch &task);
  void finishSwitch(const FinishSwitch &task);
  void nextCaseTest(const NextCaseTest &task);
  void nextCaseBody(const NextCaseBody &task);
  void addCaseFacts(const AddCaseFacts &task);
  void afterCaseTest(const AfterCaseTest &task);
  void afterCaseBody(const AfterCaseBody &task);
  void finishCaseTest(const FinishCaseTest &task);
  void afterThenBranch(const AfterThenBranch &task);
  void afterSwitchValue(const AfterSwitchValue &task);
  void afterIfCondition(const AfterIfCondition &task);
  void walkCaseStatement(const WalkCaseStatement &task);
  void finishConstantCase(const FinishConstantCase &task);

private:
  using ResultTestsComparable = Result<const Type *, CodeBranchesRegistryCaseTypeError>;
  using ResultValueComparable = Result<const Type *, CodeBranchesRegistryValueTypeError>;
  using ResultCasesAllHandled = Result<const SwitchStmt *, CodeBranchesRegistryCasesHandledError>;

  void walkExhaustive(SwitchDraft &draft);
  void finishCaseTests(SwitchDraft draft);
  void forgetCaseVars(const SwitchStmt *stmt);
  void finishCaseBodies(const SwitchDraft &draft);
  void rememberCaseVars(const SwitchCase &switchCase);
  void forgetOtherCaseVars(const VarDeclStmt *varDecl);
  void forgetCaseBodyVars(const SwitchCase &switchCase);
  void rememberOtherCaseVars(const VarDeclStmt *varDecl);
  void addComparedFacts(SwitchStmt *stmt, SwitchCase &switchCase, const Type *type);
  void walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &switchCase);
  void startConstantIf(IfStmt *stmt, bool constant, const ConditionFlows &flows);


  bool isLiteralSet(const Type *type) const;
  bool isConstantTest(const Expr *test) const;


  const Type *caseLiteral(const Expr *test) const;
  const Type *switchHint(const Type *type) const;


  ResultValueComparable checkValueType(const Type *type) const;
  ResultTestsComparable checkCaseType(const Type *type, const Type *testType) const;
  ResultCasesAllHandled checkCasesHandled(const SwitchStmt *stmt,
                                          llvm::ArrayRef<const Type *> left) const;


  const char *nameSeparator(size_t index, size_t count) const;
  std::string caseNames(llvm::ArrayRef<const Type *> left) const;
  BinaryExpr *equalTest(llvm::SMLoc loc, Expr *value, Expr *test);
  std::vector<Stmt *> caseStatements(const SwitchStmt *stmt) const;
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

  SemanticState &state;
};

}
