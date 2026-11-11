#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/program/library_ranges/library_ranges.h"
#include "compiler/program/program/program.h"
#include "compiler/result/result/result.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <set>
#include <utility>
#include <vector>

namespace tz {

class HeldValueChecker {
public:
  using ResultHeldValuesChecked = Result<void, std::vector<Diagnostic>>;

  HeldValueChecker(Program &checked, const LibraryRanges &library);
  virtual ~HeldValueChecker() = default;

  ResultHeldValuesChecked run();

protected:
  static const Expr *bare(const Expr *e);
  static bool storageMethod(const CallExpr *c, llvm::StringRef name);
  static const VarSymbol *storageOf(const CallExpr *c);
  static const FunctionDecl *callbackOf(const Expr *e);
  static const ClassDecl *storageClass(const ClassDecl *cls);

  virtual bool used() const = 0;
  virtual std::vector<Diagnostic> kept(const Expr *value, bool closure, llvm::StringRef where) = 0;
  virtual bool produces(const CallExpr *c) const = 0;
  virtual bool takesOver(const CallExpr *c) = 0;
  virtual void sawNew(const NewExpr *n) = 0;
  virtual std::vector<Diagnostic> sawVoid(const CallExpr *c) = 0;
  virtual std::vector<Diagnostic> sawCall(const CallExpr *c) = 0;
  virtual bool heldWithObject(const MemberExpr *m) const = 0;
  virtual std::vector<Diagnostic> spreadKept(const Expr *value) = 0;
  virtual size_t extraKnown() const = 0;
  virtual bool closuresCarry() const = 0;
  virtual std::vector<Diagnostic> finish() = 0;
  virtual void startWalk() = 0;

  bool once(const Expr *value);
  bool isHeld(const Expr *e) const;
  bool inLibrary(llvm::SMLoc loc) const;
  bool usesModule(llvm::StringRef name) const;
  bool carries(const FunctionDecl *fn) const;
  void markThis(const FunctionDecl *fn);
  void markParam(const FunctionDecl *fn, unsigned index);
  const FunctionDecl *programTarget(const CallExpr *c) const;

  Program &program;
  const LibraryRanges &library;
  const FunctionDecl *current;
  llvm::DenseSet<const Expr *> awaited;
  llvm::DenseSet<const Expr *> allowedNews;
  llvm::DenseSet<const Expr *> allowedStores;
  llvm::DenseSet<const VarSymbol *> heldVars;
  llvm::DenseSet<const FunctionDecl *> returnsHeld;
  llvm::DenseMap<const FunctionDecl *, std::vector<const CallExpr *>> callSites;
  llvm::DenseMap<const FunctionDecl *, std::vector<const FunctionDecl *>> callees;

private:
  enum class WalkStepKind {
    START_TOP_LEVEL,
    START_FUNCTION,
    START_PARAM,
    FINISH_FUNCTION,
    START_STMT,
    START_EXPR,
    MARK_AWAITED,
    START_CALL,
    FINISH_CALL,
    FINISH_NEW,
    FINISH_ASSIGN,
    FINISH_UNARY,
    FINISH_PROPERTY,
    FINISH_ELEMENT,
    FINISH_DECLARATOR,
    FINISH_RETURN,
  };

  struct StartTopLevel {
    static constexpr WalkStepKind kind = WalkStepKind::START_TOP_LEVEL;
    const Stmt *stmt;
  };

  struct StartFunction {
    static constexpr WalkStepKind kind = WalkStepKind::START_FUNCTION;
    const FunctionDecl *fn;
  };

  struct StartParam {
    static constexpr WalkStepKind kind = WalkStepKind::START_PARAM;
    const FunctionDecl *fn;
    unsigned index;
  };

  struct FinishFunction {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_FUNCTION;
    const FunctionDecl *outer;
  };

  struct StartStmt {
    static constexpr WalkStepKind kind = WalkStepKind::START_STMT;
    const Stmt *stmt;
  };

  struct StartExpr {
    static constexpr WalkStepKind kind = WalkStepKind::START_EXPR;
    const Expr *expr;
  };

  struct MarkAwaited {
    static constexpr WalkStepKind kind = WalkStepKind::MARK_AWAITED;
    const Expr *value;
  };

  struct StartCall {
    static constexpr WalkStepKind kind = WalkStepKind::START_CALL;
    const CallExpr *call;
  };

  struct FinishCall {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_CALL;
    const CallExpr *call;
  };

  struct FinishNew {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_NEW;
    const NewExpr *create;
  };

  struct FinishAssign {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_ASSIGN;
    const AssignExpr *assign;
  };

  struct FinishUnary {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_UNARY;
    const UnaryExpr *unary;
  };

  struct FinishProperty {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_PROPERTY;
    const ObjectLitExpr *literal;
    const ObjectLitExpr::Property *property;
  };

  struct FinishElement {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_ELEMENT;
    const ArrayLitExpr::Element *element;
  };

  struct FinishDeclarator {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_DECLARATOR;
    const VarDeclarator *declarator;
  };

  struct FinishReturn {
    static constexpr WalkStepKind kind = WalkStepKind::FINISH_RETURN;
    const Expr *value;
  };

  using WalkStep =
      Union<StartTopLevel, StartFunction, StartParam, FinishFunction, StartStmt, StartExpr,
            MarkAwaited, StartCall, FinishCall, FinishNew, FinishAssign, FinishUnary,
            FinishProperty, FinishElement, FinishDeclarator, FinishReturn>;

  static std::string calleeName(const CallExpr *c);

  std::vector<Diagnostic> walkProgram();
  std::vector<WalkStep> programRoots() const;
  std::vector<WalkStep> classRoots(ClassDecl *cls) const;
  std::vector<Diagnostic> applyStep(const WalkStep &step, std::vector<WalkStep> &steps);
  void startTopLevel(const Stmt *s, std::vector<WalkStep> &steps);
  void startFunction(const FunctionDecl *fn, std::vector<WalkStep> &steps);
  void startParam(const StartParam &step, std::vector<WalkStep> &steps);
  void startStmt(const Stmt *s, std::vector<WalkStep> &steps) const;
  void startExpr(const Expr *e, std::vector<WalkStep> &steps) const;
  std::vector<WalkStep> stmtParts(const Stmt *s) const;
  std::vector<WalkStep> blockParts(const BlockStmt *block) const;
  std::vector<WalkStep> declaratorParts(const VarDeclStmt *decl) const;
  std::vector<WalkStep> switchParts(const SwitchStmt *s) const;
  std::vector<WalkStep> classParts(const ClassDecl *cls) const;
  std::vector<WalkStep> exprParts(const Expr *e) const;
  std::vector<WalkStep> exprSteps(llvm::ArrayRef<Expr *> exprs) const;
  std::vector<WalkStep> callParts(const CallExpr *c) const;
  std::vector<WalkStep> newParts(const NewExpr *n) const;
  std::vector<WalkStep> objectParts(const ObjectLitExpr *literal) const;
  std::vector<WalkStep> arrayParts(const ArrayLitExpr *literal) const;
  std::vector<Diagnostic> finishCall(const CallExpr *c);
  std::vector<Diagnostic> finishNew(const NewExpr *n);
  std::vector<Diagnostic> finishAssign(const AssignExpr *a);
  std::vector<Diagnostic> finishUnary(const UnaryExpr *unary);
  std::vector<Diagnostic> finishProperty(const FinishProperty &step);
  std::vector<Diagnostic> finishElement(const ArrayLitExpr::Element *element);
  std::vector<Diagnostic> finishDeclarator(const VarDeclarator *d);
  void finishReturn(const Expr *value);
  std::vector<Diagnostic> keep(const Expr *value, llvm::StringRef where);
  std::vector<Diagnostic> passed(const CallExpr *c, const FunctionDecl *target,
                                 llvm::StringRef name);
  bool isHeldItself(const Expr *e) const;
  std::vector<const Expr *> heldParts(const Expr *e) const;

  bool reporting;
  llvm::DenseSet<const char *> reported;
  llvm::DenseSet<const FunctionDecl *> walked;
  llvm::DenseSet<const FunctionDecl *> heldThis;
  std::set<std::pair<const FunctionDecl *, unsigned>> heldParams;
};

}
