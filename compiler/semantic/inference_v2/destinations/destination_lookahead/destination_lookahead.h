#pragma once

#include "compiler/semantic/inference_v2/destinations/destination_peeks/destination_peeks.h"
#include "compiler/semantic/inference_v2/destinations/destination_search/destination_search.h"
#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task.h"
#include "compiler/union/union/union.h"
#include <deque>
#include <vector>

namespace tz {

class SemanticState;

enum class LookStepKind {
  START_LIST,
  LIST_ITEM,
  STMT,
  EXPR,
  VAR_DECLARE,
  DECLARE_NAME,
  LEAVE_SCOPE,
  FOR_REST,
  FOR_OF_REST,
  FUNCTION,
  PARAM,
  FUNCTION_END,
  ASSIGN_VALUE,
};

struct LookList {
  static constexpr LookStepKind kind = LookStepKind::START_LIST;
  llvm::ArrayRef<Stmt *> stmts;
};

struct LookItem {
  static constexpr LookStepKind kind = LookStepKind::LIST_ITEM;
  llvm::ArrayRef<Stmt *> stmts;
  size_t index;
};

struct LookStmt {
  static constexpr LookStepKind kind = LookStepKind::STMT;
  const Stmt *stmt;
};

struct LookExpr {
  static constexpr LookStepKind kind = LookStepKind::EXPR;
  const Expr *expr;
  const Type *sink;
};

struct LookDeclarator {
  static constexpr LookStepKind kind = LookStepKind::VAR_DECLARE;
  const VarDeclarator *declarator;
};

struct LookName {
  static constexpr LookStepKind kind = LookStepKind::DECLARE_NAME;
  llvm::StringRef name;
  const Type *type;
};

struct LookScopeEnd {
  static constexpr LookStepKind kind = LookStepKind::LEAVE_SCOPE;
};

struct LookForRest {
  static constexpr LookStepKind kind = LookStepKind::FOR_REST;
  const ForStmt *loop;
};

struct LookForOfRest {
  static constexpr LookStepKind kind = LookStepKind::FOR_OF_REST;
  const ForOfStmt *loop;
};

struct LookFunction {
  static constexpr LookStepKind kind = LookStepKind::FUNCTION;
  const FunctionDecl *functionDecl;
};

struct LookParam {
  static constexpr LookStepKind kind = LookStepKind::PARAM;
  const Param *param;
};

struct LookFunctionEnd {
  static constexpr LookStepKind kind = LookStepKind::FUNCTION_END;
  const Type *outerReturns;
};

struct LookAssign {
  static constexpr LookStepKind kind = LookStepKind::ASSIGN_VALUE;
  const AssignExpr *assign;
};

using LookStep =
    Union<LookList, LookItem, LookStmt, LookExpr, LookDeclarator, LookName, LookScopeEnd,
          LookForRest, LookForOfRest, LookFunction, LookParam, LookFunctionEnd, LookAssign>;


class DestinationLookahead {
public:
  DestinationLookahead(SemanticState &owner, DestinationSearch &found, DestinationTypes &known);

  void scanStatement(const Stmt *stmt);
  void scanValue(const Expr *expr, const Type *sink);


  const Type *annotatedType(const VarDeclarator &declarator);

private:
  void runScan(LookStep first);
  void scanStmt(const Stmt *stmt);
  void forRest(const ForStmt *loop);
  void scanParam(const Param *param);
  void listItem(const LookItem &item);
  void applyStep(const LookStep &step);
  void startList(const LookList &list);
  void forOfRest(const ForOfStmt *loop);
  void pushForParts(const ForStmt *loop);
  void pushIfParts(const IfStmt *ifStmt);
  void scanExpr(const LookExpr &scanned);
  void assignValue(const AssignExpr *assign);
  void functionEnd(const Type *outerReturns);
  void pushForOfParts(const ForOfStmt *loop);
  void pushWhileParts(const WhileStmt *loop);
  void scanSwitch(const SwitchStmt *switchStmt);
  void pushDoWhileParts(const DoWhileStmt *loop);
  void pushDeclarators(const VarDeclStmt *varDecl);
  void varDeclare(const VarDeclarator *declarator);
  void pushReturnValue(const ReturnStmt *returnStmt);
  void scanFunction(const FunctionDecl *functionDecl);


  bool isDeclaringName(const Stmt *stmt, llvm::StringRef name) const;
  bool hasParamNamed(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  bool isDeclaringFunction(llvm::ArrayRef<Stmt *> stmts, llvm::StringRef name) const;


  const Type *writtenParamType(const Param &param);
  const Type *knownType(const TypeRef &ref, TypeUse use);
  const Type *writtenReturnType(const FunctionDecl *functionDecl);
  const Type *declaredFunctionType(const FunctionDecl *functionDecl);
  const Type *plainAssignType(const AssignExpr *assign, const Expr *side) const;
  const Type *propertySink(const ObjectLitExpr::Property &property, const Type *object) const;
  const Type *elementSink(const ArrayLitExpr::Element &element, size_t index,
                          const Type *object) const;


  std::vector<LookExpr> callParts(const CallExpr *call);
  std::vector<LookExpr> newParts(const NewExpr *create);
  std::vector<LookExpr> assignParts(const AssignExpr *assign);
  std::vector<LookExpr> exprParts(const Expr *expr, const Type *sink);
  std::vector<LookExpr> templateParts(const TemplateExpr *templateExpr) const;
  std::vector<LookExpr> nameParts(const NameExpr *name, const Type *sink) const;
  std::vector<LookExpr> binaryParts(const BinaryExpr *binary, const Type *sink) const;
  std::vector<LookExpr> arrayParts(const ArrayLitExpr *literal, const Type *object) const;
  std::vector<LookExpr> objectParts(const ObjectLitExpr *literal, const Type *object) const;
  std::vector<LookExpr> sequenceParts(const SequenceExpr *sequence, const Type *sink) const;
  std::vector<LookExpr> conditionalParts(const ConditionalExpr *conditional,
                                         const Type *sink) const;


  LookExpr untyped(const Expr *expr) const;

  SemanticState &state;
  DestinationSearch &search;
  DestinationTypes &types;
  DestinationPeeks peeks;
  std::vector<LookStep> steps;
  std::deque<std::vector<Stmt *>> caseBodies;
};

}
