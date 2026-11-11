#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/switch_checker/switch_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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

  void checkSwitch(SwitchStmt *stmt);

private:
  using ResultValueComparable = Result<const Type *, SwitchCheckerValueTypeError>;
  using ResultTestsComparable = Result<const Type *, SwitchCheckerCaseTypeError>;
  using ResultCasesAllHandled = Result<const SwitchStmt *, SwitchCheckerCasesHandledError>;

  static bool isConstantTest(const Expr *test);
  static ResultValueComparable valueType(const Type *type);
  static ResultTestsComparable caseType(const Type *type, const Type *testType);
  static ResultCasesAllHandled casesHandled(const SwitchStmt *stmt,
                                            llvm::ArrayRef<const Type *> left);

  struct SwitchFlows {
    const Type *type;
    const Type *hint;
    Flow tests;
    std::vector<Flow> matched;
  };

  static std::vector<Stmt *> caseStatements(const SwitchStmt *stmt);
  static const Type *switchHint(const Type *type);
  void walkCaseTest(SwitchStmt *stmt, size_t i, SwitchFlows &flows);
  void walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &c);
  void addCaseFacts(SwitchStmt *stmt, SwitchCase &c, const Type *type, Flow &whenTrue,
                    Flow &whenFalse);
  void addComparedFacts(SwitchStmt *stmt, SwitchCase &c, const Type *type, Flow &whenTrue,
                        Flow &whenFalse);
  void walkExhaustive(SwitchStmt *stmt, SwitchFlows &flows);
  Flow caseBodiesExit(SwitchStmt *stmt, const SwitchFlows &flows, const std::vector<Stmt *> &all);
  void rememberCaseVars(const SwitchCase &c);
  void forgetCaseVars(const SwitchStmt *stmt);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  StmtChecker *stmtChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  UnionChecker *unionChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
};

}
