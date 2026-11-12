#pragma once

#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"
#include <deque>

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class Destination;
class TypeResolver;
class TypeRelations;
class ClassDeclarer;
class IterableChecker;
class PredicateChecker;

struct ScopeEntry;
class DestinationScan;

enum class ScanStepKind {
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

struct StartList {
  static constexpr ScanStepKind kind = ScanStepKind::START_LIST;
  llvm::ArrayRef<Stmt *> stmts;
};

struct ListItem {
  static constexpr ScanStepKind kind = ScanStepKind::LIST_ITEM;
  llvm::ArrayRef<Stmt *> stmts;
  size_t index;
};

struct ScanStmt {
  static constexpr ScanStepKind kind = ScanStepKind::STMT;
  const Stmt *stmt;
};

struct ScanExpr {
  static constexpr ScanStepKind kind = ScanStepKind::EXPR;
  const Expr *expr;
  const Type *sink;
};

struct VarDeclare {
  static constexpr ScanStepKind kind = ScanStepKind::VAR_DECLARE;
  const VarDeclarator *declarator;
};

struct DeclareName {
  static constexpr ScanStepKind kind = ScanStepKind::DECLARE_NAME;
  llvm::StringRef name;
  const Type *type;
};

struct LeaveScope {
  static constexpr ScanStepKind kind = ScanStepKind::LEAVE_SCOPE;
};

struct ForRest {
  static constexpr ScanStepKind kind = ScanStepKind::FOR_REST;
  const ForStmt *loop;
};

struct ForOfRest {
  static constexpr ScanStepKind kind = ScanStepKind::FOR_OF_REST;
  const ForOfStmt *loop;
};

struct ScanFunction {
  static constexpr ScanStepKind kind = ScanStepKind::FUNCTION;
  const FunctionDecl *functionDecl;
};

struct ScanParam {
  static constexpr ScanStepKind kind = ScanStepKind::PARAM;
  const Param *param;
};

struct FunctionEnd {
  static constexpr ScanStepKind kind = ScanStepKind::FUNCTION_END;
  const Type *outerReturns;
};

struct AssignValue {
  static constexpr ScanStepKind kind = ScanStepKind::ASSIGN_VALUE;
  const AssignExpr *assign;
};

using ScanStep = Union<StartList, ListItem, ScanStmt, ScanExpr, VarDeclare, DeclareName, LeaveScope,
                       ForRest, ForOfRest, ScanFunction, ScanParam, FunctionEnd, AssignValue>;

struct ScanRun {
  DestinationScan &scan;
  std::vector<ScanStep> steps;
  std::deque<std::vector<Stmt *>> caseBodies;
};

class DestinationScanner {
public:
  DestinationScanner(SemanticState &owner, SemanticParts &all);

  bool isResultLiteral(const CallExpr *call) const;
  void scanDestinations(const Stmt *stmt, DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan);

private:
  void runScan(ScanStep first, DestinationScan &scan);
  void applyStep(const ScanStep &step, ScanRun &run);

  void scanStmt(const Stmt *stmt, ScanRun &run);
  void scanParam(const Param *param, ScanRun &run);
  void scanExpr(const ScanExpr &scanned, ScanRun &run);
  void startList(const StartList &list, ScanRun &run);
  void listItem(const ListItem &item, ScanRun &run) const;
  void forRest(const ForStmt *loop, ScanRun &run) const;
  void assignValue(const AssignExpr *assign, ScanRun &run);
  void forOfRest(const ForOfStmt *loop, ScanRun &run) const;
  void pushIfParts(const IfStmt *ifStmt, ScanRun &run) const;
  void pushForParts(const ForStmt *loop, ScanRun &run) const;
  void varDeclare(const VarDeclarator *declarator, ScanRun &run);
  void functionEnd(const Type *outerReturns, ScanRun &run) const;
  void pushWhileParts(const WhileStmt *loop, ScanRun &run) const;
  void pushForOfParts(const ForOfStmt *loop, ScanRun &run) const;
  void scanSwitch(const SwitchStmt *switchStmt, ScanRun &run) const;
  void scanFunction(const FunctionDecl *functionDecl, ScanRun &run);
  void pushDoWhileParts(const DoWhileStmt *loop, ScanRun &run) const;
  void pushDeclarators(const VarDeclStmt *varDecl, ScanRun &run) const;
  void pushReturnValue(const ReturnStmt *returnStmt, ScanRun &run) const;

  ScanExpr untyped(const Expr *expr) const;
  std::vector<ScanExpr> newParts(const NewExpr *create, ScanRun &run);
  std::vector<ScanExpr> callParts(const CallExpr *call, ScanRun &run);
  std::vector<ScanExpr> assignParts(const AssignExpr *assign, ScanRun &run);
  std::vector<ScanExpr> templateParts(const TemplateExpr *templateExpr) const;
  std::vector<ScanExpr> exprParts(const Expr *expr, const Type *sink, ScanRun &run);
  std::vector<ScanExpr> binaryParts(const BinaryExpr *binary, const Type *sink) const;
  std::vector<ScanExpr> arrayParts(const ArrayLitExpr *literal, const Type *object) const;
  std::vector<ScanExpr> objectParts(const ObjectLitExpr *literal, const Type *object) const;
  std::vector<ScanExpr> sequenceParts(const SequenceExpr *sequence, const Type *sink) const;
  std::vector<ScanExpr> nameParts(const NameExpr *name, const Type *sink, ScanRun &run) const;
  std::vector<ScanExpr> conditionalParts(const ConditionalExpr *conditional,
                                         const Type *sink) const;

  const Type *peekSubtle() const;
  const Type *writtenParamType(const Param &param);
  const Type *elementPeekType(const Type *object) const;
  NodeChildren<const Expr *> peekParts(const Expr *expr) const;
  const FuncSymbol *calledFunction(const ScopeEntry &entry) const;
  const Type *writtenReturnType(const FunctionDecl *functionDecl);
  const Type *choicePeekType(std::vector<const Type *> &answers) const;
  bool isDeclaringName(const Stmt *stmt, llvm::StringRef name) const;
  bool hasFunctionField(const Type *object, llvm::StringRef name) const;
  const Type *peekType(const Expr *expr, const DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const DestinationScan &scan) const;
  const Type *memberPeekType(const MemberExpr *member, const Type *object) const;
  bool hasParamNamed(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  const Type *namePeekType(const NameExpr *name, const DestinationScan &scan) const;
  bool isDeclaringFunction(llvm::ArrayRef<Stmt *> stmts, llvm::StringRef name) const;
  const ClassDecl *namedClass(const Expr *object, const DestinationScan &scan) const;
  std::vector<const Type *> functionTypeParams(const Type *type, size_t argCount) const;
  const Type *resultHalf(const CallExpr *call, std::vector<const Type *> &answers) const;
  std::vector<const Type *> calleeParams(const CallExpr *call, const DestinationScan &scan);
  const FuncSymbol *soleConstructor(const NewExpr *create, const DestinationScan &scan) const;
  const Type *resultPeekType(const CallExpr *call, std::vector<const Type *> &answers) const;
  std::vector<const Type *> dictMethodParams(const Type *object, llvm::StringRef name) const;
  const Type *propertySink(const ObjectLitExpr::Property &property, const Type *object) const;
  const Type *plainAssignType(const AssignExpr *assign, const Expr *side,
                              const DestinationScan &scan) const;
  const Type *elementSink(const ArrayLitExpr::Element &element, size_t index,
                          const Type *object) const;
  const Type *nodePeekType(const Expr *expr, const DestinationScan &scan,
                           std::vector<const Type *> &answers) const;
  std::vector<const Type *> arrayMethodParams(const Type *array, llvm::StringRef name,
                                              size_t argCount) const;
  std::vector<const Type *> spreadParams(llvm::ArrayRef<const Type *> params, bool hasRest,
                                         size_t argCount) const;
  std::vector<const Type *> soleMethodParams(llvm::ArrayRef<FunctionDecl *> methods,
                                             size_t argCount) const;
  std::vector<const Type *> nameCalleeParams(const CallExpr *call, const NameExpr *name,
                                             const DestinationScan &scan);
  std::vector<const Type *> memberCalleeParams(const CallExpr *call, const MemberExpr *member,
                                               const DestinationScan &scan);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  Destination *destination = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  IterableChecker *iterableChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
