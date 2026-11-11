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
class IterableChecker;
class PredicateChecker;

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
  const VarDeclarator *d;
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
  const FunctionDecl *fn;
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
  static bool isResultLiteral(const CallExpr *call);
  DestinationScanner(SemanticState &owner, SemanticParts &all);

  void scanDestinations(const Stmt *stmt, DestinationScan &scan);
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan);
  const Type *peekType(const Expr *expr, const DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const DestinationScan &scan) const;
  std::vector<const Type *> calleeParams(const CallExpr *call, const DestinationScan &scan);

private:
  static NodeChildren<const Expr *> peekParts(const Expr *expr);
  static const Type *resultPeekType(const CallExpr *call, std::vector<const Type *> &answers);
  const Type *nodePeekType(const Expr *expr, const DestinationScan &scan,
                           std::vector<const Type *> &answers) const;
  const Type *memberPeekType(const MemberExpr *member, const Type *object) const;
  static void listItem(const ListItem &item, ScanRun &run);
  static bool declaresName(const Stmt *stmt, llvm::StringRef name);
  static void scanSwitch(const SwitchStmt *s, ScanRun &run);
  static void forRest(const ForStmt *loop, ScanRun &run);
  static void forOfRest(const ForOfStmt *loop, ScanRun &run);
  static void functionEnd(const Type *outerReturns, ScanRun &run);
  static std::vector<ScanExpr> arrayParts(const ArrayLitExpr *lit, const Type *object);
  static void pushIfParts(const IfStmt *s, ScanRun &run);
  static void pushForParts(const ForStmt *s, ScanRun &run);
  static void pushWhileParts(const WhileStmt *s, ScanRun &run);
  static void pushForOfParts(const ForOfStmt *s, ScanRun &run);
  static void pushReturnValue(const ReturnStmt *s, ScanRun &run);
  static void pushDoWhileParts(const DoWhileStmt *s, ScanRun &run);
  static void pushDeclarators(const VarDeclStmt *decl, ScanRun &run);
  static std::vector<ScanExpr> templateParts(const TemplateExpr *tmpl);
  static std::vector<ScanExpr> binaryParts(const BinaryExpr *bin, const Type *sink);
  static std::vector<ScanExpr> objectParts(const ObjectLitExpr *lit, const Type *object);
  static std::vector<ScanExpr> sequenceParts(const SequenceExpr *seq, const Type *sink);
  static std::vector<ScanExpr> nameParts(const NameExpr *name, const Type *sink, ScanRun &run);
  static std::vector<ScanExpr> conditionalParts(const ConditionalExpr *cond, const Type *sink);
  static std::vector<const Type *> dictMethodParams(const Type *object, llvm::StringRef name);
  static std::vector<const Type *> arrayMethodParams(const Type *array, llvm::StringRef name,
                                                     size_t argCount);
  static std::vector<const Type *> spreadParams(llvm::ArrayRef<const Type *> params, bool hasRest,
                                                size_t argCount);
  static std::vector<const Type *> methodParams(const ClassDecl *cls, llvm::StringRef name,
                                                bool isStatic, size_t argCount);
  std::vector<const Type *> nameCalleeParams(const CallExpr *call, const NameExpr *name,
                                             const DestinationScan &scan);
  std::vector<const Type *> memberCalleeParams(const CallExpr *call, const MemberExpr *member,
                                               const DestinationScan &scan);

  void runScan(ScanStep first, DestinationScan &scan);
  void applyStep(const ScanStep &step, ScanRun &run);
  void startList(const StartList &list, ScanRun &run);
  void scanStmt(const Stmt *stmt, ScanRun &run);
  void varDeclare(const VarDeclarator *d, ScanRun &run);
  void scanFunction(const FunctionDecl *fn, ScanRun &run);
  void scanParam(const Param *param, ScanRun &run);
  void assignValue(const AssignExpr *assign, ScanRun &run);
  void scanExpr(const ScanExpr &scanned, ScanRun &run);
  std::vector<ScanExpr> exprParts(const Expr *expr, const Type *sink, ScanRun &run);
  std::vector<ScanExpr> assignParts(const AssignExpr *assign, ScanRun &run);
  std::vector<ScanExpr> callParts(const CallExpr *call, ScanRun &run);
  std::vector<ScanExpr> newParts(const NewExpr *create, ScanRun &run);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  Destination *destination = nullptr;
  TypeResolver *typeResolver = nullptr;
  IterableChecker *iterableChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
