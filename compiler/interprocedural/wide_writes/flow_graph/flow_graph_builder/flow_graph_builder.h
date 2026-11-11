#pragma once

#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/types/types.h"
#include "compiler/program/widening_store/widening_store.h"
#include "compiler/semantic/semantic_types.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace tz {

struct FlowRun {
  enum class StepKind {
    WALK_STMT,
    WALK_FUNCTION,
    WALK_CLASS,
    LEAVE_FUNCTION,
    WALK_EXPR,
    DROP_ANSWER,
    EMPTY_ANSWER,
    BIND_VAR,
    BIND_FIELD,
    BIND_FOR_OF,
    FINISH_RETURN,
    FINISH_ARRAY,
    FINISH_PROPERTY,
    PASS_OBJECT_THIS,
    FINISH_FILTER,
    FINISH_MEMBER,
    FINISH_INDEX,
    FINISH_JOIN,
    START_ASSIGN,
    FINISH_ASSIGN,
    FINISH_CALL,
    FINISH_NEW,
    FINISH_CAST,
    FINISH_FUNCTION_EXPR,
    FINISH_YIELD,
    FINISH_AWAIT,
  };

  struct AssignTarget {
    Expr *target = nullptr;
    const Expr *valueExpr = nullptr;
    bool isCompound = false;
    bool isUpdate = false;
    const Expr *whole = nullptr;
  };

  struct WalkStmt {
    static constexpr StepKind kind = StepKind::WALK_STMT;
    Stmt *stmt;
  };

  struct WalkFunction {
    static constexpr StepKind kind = StepKind::WALK_FUNCTION;
    FunctionDecl *fn;
  };

  struct WalkClass {
    static constexpr StepKind kind = StepKind::WALK_CLASS;
    ClassDecl *cls;
  };

  struct LeaveFunction {
    static constexpr StepKind kind = StepKind::LEAVE_FUNCTION;
  };

  struct WalkExpr {
    static constexpr StepKind kind = StepKind::WALK_EXPR;
    Expr *expr;
  };

  struct DropAnswer {
    static constexpr StepKind kind = StepKind::DROP_ANSWER;
  };

  struct EmptyAnswer {
    static constexpr StepKind kind = StepKind::EMPTY_ANSWER;
  };

  struct BindVar {
    static constexpr StepKind kind = StepKind::BIND_VAR;
    const VarSymbol *var;
  };

  struct BindField {
    static constexpr StepKind kind = StepKind::BIND_FIELD;
    ClassField *field;
  };

  struct BindForOf {
    static constexpr StepKind kind = StepKind::BIND_FOR_OF;
    ForOfStmt *loop;
  };

  struct FinishReturn {
    static constexpr StepKind kind = StepKind::FINISH_RETURN;
  };

  struct FinishArray {
    static constexpr StepKind kind = StepKind::FINISH_ARRAY;
    ArrayLitExpr *literal;
  };

  struct FinishProperty {
    static constexpr StepKind kind = StepKind::FINISH_PROPERTY;
    ObjectLitExpr::Property *property;
  };

  struct PassObjectThis {
    static constexpr StepKind kind = StepKind::PASS_OBJECT_THIS;
    FunctionDecl *fn;
  };

  struct FinishFilter {
    static constexpr StepKind kind = StepKind::FINISH_FILTER;
    const Expr *expr;
  };

  struct FinishMember {
    static constexpr StepKind kind = StepKind::FINISH_MEMBER;
    MemberExpr *member;
  };

  struct FinishIndex {
    static constexpr StepKind kind = StepKind::FINISH_INDEX;
    IndexExpr *index;
  };

  struct FinishJoin {
    static constexpr StepKind kind = StepKind::FINISH_JOIN;
    const Expr *expr;
  };

  struct StartAssign {
    static constexpr StepKind kind = StepKind::START_ASSIGN;
    AssignTarget assign;
  };

  struct FinishAssign {
    static constexpr StepKind kind = StepKind::FINISH_ASSIGN;
    AssignTarget assign;
  };

  struct FinishCall {
    static constexpr StepKind kind = StepKind::FINISH_CALL;
    CallExpr *call;
  };

  struct FinishNew {
    static constexpr StepKind kind = StepKind::FINISH_NEW;
    NewExpr *create;
  };

  struct FinishCast {
    static constexpr StepKind kind = StepKind::FINISH_CAST;
    CastExpr *conversion;
  };

  struct FinishFunctionExpr {
    static constexpr StepKind kind = StepKind::FINISH_FUNCTION_EXPR;
    FunctionDecl *fn;
  };

  struct FinishYield {
    static constexpr StepKind kind = StepKind::FINISH_YIELD;
    YieldExpr *yield;
  };

  struct FinishAwait {
    static constexpr StepKind kind = StepKind::FINISH_AWAIT;
    AwaitExpr *await;
  };

  using Step = Union<WalkStmt, WalkFunction, WalkClass, LeaveFunction, WalkExpr, DropAnswer,
                     EmptyAnswer, BindVar, BindField, BindForOf, FinishReturn, FinishArray,
                     FinishProperty, PassObjectThis, FinishFilter, FinishMember, FinishIndex,
                     FinishJoin, StartAssign, FinishAssign, FinishCall, FinishNew, FinishCast,
                     FinishFunctionExpr, FinishYield, FinishAwait>;

  std::vector<Step> steps;
  std::vector<Refs> answers;
};

class FlowStmts;
class FlowCalls;
class FlowBuiltins;
class FlowExprs;

class FlowGraphBuilder {
public:
  using NarrowingsOf = llvm::function_ref<std::vector<NarrowedPart>(const Type *, const Type *)>;
  using ConvertibleTo = llvm::function_ref<bool(const Type *, const Type *)>;

  FlowGraphBuilder(const WideningStore &wideningStore, NarrowingsOf narrowingsOf,
                   ConvertibleTo convertibleTo);
  ~FlowGraphBuilder();

  FlowGraph buildGraph(SourceFile &file);
  void walk(FlowRun::Step first);
  void pushSteps(FlowRun &run, const std::vector<FlowRun::Step> &parts) const;
  void pushAnswer(FlowRun &run, const Expr *expr, Refs refs) const;
  Refs takeAnswer(FlowRun &run) const;
  std::vector<Refs> takeAnswers(FlowRun &run, size_t count) const;

  FlowStmts &getFlowStmts() const;
  FlowCalls &getFlowCalls() const;
  FlowExprs &getFlowExprs() const;
  FlowBuiltins &getFlowBuiltins() const;

  int getUnknownNode() const;
  int fieldNode(llvm::StringRef name);
  int varNode(const VarSymbol *var);
  int returnNode(const FunctionDecl *fn);
  int contentsNode(const Type *type);
  int findNode(NodeKind kind, const void *key, llvm::StringRef name);

  void escape(const Refs &refs);
  void addFlow(const Refs &refs, int to);
  void addWrite(const Write &write);
  void addConversion(const Conversion &conversion);

  bool isConvertible(const Type *from, const Type *to) const;
  bool isWideningCast(const CastExpr *cast) const;
  std::vector<NarrowedPart> findNarrowings(const Type *from, const Type *to) const;

  void leaveFunction();
  bool isInsideFunction() const;
  void markWalked(const FunctionDecl *fn);
  void enterFunction(const FunctionDecl *fn);
  bool isWalked(const FunctionDecl *fn) const;
  const VarSymbol *findCurrentThis() const;
  const FunctionDecl *getCurrentFunction() const;

  void rememberValueCall(const ValueCall &call);
  void markRuntimeFunction(const VarSymbol *var);
  bool isRuntimeFunction(const VarSymbol *var) const;
  void rememberFunctionValue(const FunctionDecl *fn);

  static Refs copied(const Refs &refs);
  static Refs holding(const Refs &refs);
  static void append(Refs &to, const Refs &from);
  static Refs shifted(const Refs &refs, int levels);
  static const Type *ownType(const Expr *expr);
  static bool holdsReferences(const Type *type);

private:
  int markHolder(int id, const Type *type);
  void applyStep(const FlowRun::Step &step, FlowRun &run);

  FlowGraph graph;
  NarrowingsOf narrowings;
  ConvertibleTo convertible;
  std::vector<bool> plainNodes;
  std::vector<ValueCall> valueCalls;
  const WideningStore &widenings;
  std::set<const FunctionDecl *> walked;
  std::unique_ptr<FlowStmts> flowStmts;
  std::unique_ptr<FlowCalls> flowCalls;
  std::unique_ptr<FlowExprs> flowExprs;
  std::set<const FunctionDecl *> functionSet;
  std::unique_ptr<FlowBuiltins> flowBuiltins;
  std::set<const VarSymbol *> runtimeFunctions;
  std::vector<const FunctionDecl *> functions;
  std::vector<const FunctionDecl *> functionValues;
  std::map<std::tuple<int, const void *, std::string>, int> ids;
};

}
