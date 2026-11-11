#pragma once

#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"
#include "compiler/union/union/union.h"

namespace tz {

class ProgramScan {
public:
  explicit ProgramScan(OverflowCheck &check);

  void scanProgram();
  void addFunction(const FunctionDecl *fn);
  void declareVar(const VarSymbol *var, const VarDeclarator *decl = nullptr, bool isConst = false);
  void useVar(const VarSymbol *var, bool consumed, bool write);
  bool isConstantTree(const Expr *expr) const;

private:
  enum class ScanStepKind {
    SCAN_STMT,
    SCAN_EXPR,
    SCAN_CLASS,
    DECLARE_VAR,
    DECLARE_PARAM,
    FINISH_DECLARATOR,
    INLINE_CALLBACK,
    ADD_FUNCTION,
    USE_VAR,
  };

  struct ScanStmt {
    static constexpr ScanStepKind kind = ScanStepKind::SCAN_STMT;
    const Stmt *stmt;
  };

  struct ScanExpr {
    static constexpr ScanStepKind kind = ScanStepKind::SCAN_EXPR;
    const Expr *expr;
    bool consumed;
  };

  struct ScanClass {
    static constexpr ScanStepKind kind = ScanStepKind::SCAN_CLASS;
    const ClassDecl *cls;
  };

  struct DeclareVar {
    static constexpr ScanStepKind kind = ScanStepKind::DECLARE_VAR;
    const VarSymbol *var;
    const VarDeclarator *decl;
    bool isConst;
  };

  struct DeclareParam {
    static constexpr ScanStepKind kind = ScanStepKind::DECLARE_PARAM;
    const VarSymbol *var;
  };

  struct FinishDeclarator {
    static constexpr ScanStepKind kind = ScanStepKind::FINISH_DECLARATOR;
    const VarDeclarator *declarator;
    bool isConst;
  };

  struct InlineCallback {
    static constexpr ScanStepKind kind = ScanStepKind::INLINE_CALLBACK;
    const FunctionExpr *callback;
    Builtin builtin;
  };

  struct AddFunction {
    static constexpr ScanStepKind kind = ScanStepKind::ADD_FUNCTION;
    const FunctionDecl *fn;
  };

  struct UseVar {
    static constexpr ScanStepKind kind = ScanStepKind::USE_VAR;
    const VarSymbol *var;
    bool consumed;
    bool write;
  };

  using ScanStep = Union<ScanStmt, ScanExpr, ScanClass, DeclareVar, DeclareParam, FinishDeclarator,
                         InlineCallback, AddFunction, UseVar>;

  bool onlyReads(Builtin builtin) const;
  void scanFunction(const FunctionDecl *fn);
  void runScan(const std::vector<ScanStep> &roots);
  void applyStep(const ScanStep &step, std::vector<ScanStep> &steps);
  void pushParts(std::vector<ScanStep> &steps, const std::vector<ScanStep> &parts) const;
  void declareParam(const VarSymbol *var);
  void finishDeclarator(const FinishDeclarator &step);
  std::vector<ScanStep> classParts(const ClassDecl *cls);
  std::vector<ScanStep> stmtParts(const Stmt *stmt);
  std::vector<ScanStep> declaratorParts(const VarDeclStmt *decl) const;
  std::vector<ScanStep> forParts(const ForStmt *loop) const;
  std::vector<ScanStep> switchParts(const SwitchStmt *choice) const;
  std::vector<ScanStep> exprParts(const Expr *expr, bool consumed);
  std::vector<ScanStep> nameParts(const NameExpr *name, bool consumed);
  std::vector<ScanStep> assignParts(const AssignExpr *assign);
  std::vector<ScanStep> callParts(const CallExpr *call);
  std::vector<ScanStep> inlineParts(const InlineCallback &step);

  OverflowCheck &check;
};

}
