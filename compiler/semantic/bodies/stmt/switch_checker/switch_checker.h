#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/switch_checker/switch_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class SemanticParts;
class Scopes;
class ExprChecker;
class JumpChecker;
class StmtChecker;
class TypeDeclarer;
class UnionChecker;
class BinaryChecker;
class TypeRelations;
class ConditionFacts;

class SwitchChecker {
public:
  SwitchChecker(SemanticState &owner, SemanticParts &all);

  void walkSwitch(SwitchStmt *stmt);

private:
  using ResultValueComparable = Result<const Type *, SwitchCheckerValueTypeError>;
  using ResultTestsComparable = Result<const Type *, SwitchCheckerCaseTypeError>;
  using ResultCasesAllHandled = Result<const SwitchStmt *, SwitchCheckerCasesHandledError>;

  bool isConstantTest(const Expr *test) const;
  ResultValueComparable checkValueType(const Type *type) const;
  ResultTestsComparable checkCaseType(const Type *type, const Type *testType) const;
  ResultCasesAllHandled checkCasesHandled(const SwitchStmt *stmt,
                                          llvm::ArrayRef<const Type *> left) const;
  std::string caseNames(llvm::ArrayRef<const Type *> left) const;
  const char *nameSeparator(size_t index, size_t count) const;

  struct SwitchFlows {
    const Type *type;
    const Type *hint;
    Flow tests;
    std::vector<Flow> matched;
  };

  std::vector<Stmt *> caseStatements(const SwitchStmt *stmt) const;
  const Type *switchHint(const Type *type) const;
  void walkCaseTest(SwitchStmt *stmt, size_t index, SwitchFlows &flows);
  void walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &switchCase);
  void addCaseFacts(SwitchStmt *stmt, SwitchCase &switchCase, const Type *type, Flow &whenTrue,
                    Flow &whenFalse);
  void addComparedFacts(SwitchStmt *stmt, SwitchCase &switchCase, const Type *type, Flow &whenTrue,
                        Flow &whenFalse);
  void walkExhaustive(SwitchStmt *stmt, SwitchFlows &flows);
  Flow caseBodiesExit(SwitchStmt *stmt, const SwitchFlows &flows, const std::vector<Stmt *> &all);
  size_t walkCaseBody(const SwitchCase &switchCase, const std::vector<Stmt *> &all,
                      size_t position);
  void rememberCaseVars(const SwitchCase &switchCase);
  void rememberOtherCaseVars(const VarDeclStmt *varDecl);
  void forgetCaseVars(const SwitchStmt *stmt);
  void forgetCaseBodyVars(const SwitchCase &switchCase);
  void forgetOtherCaseVars(const VarDeclStmt *varDecl);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  StmtChecker *stmtChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  UnionChecker *unionChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
};

}
