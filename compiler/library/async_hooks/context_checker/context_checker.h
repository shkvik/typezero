#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <set>
#include <utility>
#include <vector>

namespace tz {

class ContextChecker {
public:
  explicit ContextChecker(Program &program);

  void run();

private:
  bool usesStorage() const;
  bool inLibrary(llvm::SMLoc loc) const;
  void walkProgram();
  void function(const FunctionDecl *fn);
  void topLevel(const Stmt *s);
  void stmt(const Stmt *s);
  void expr(const Expr *e);
  void call(const CallExpr *c);
  void create(const NewExpr *n);
  void assign(const AssignExpr *a);
  void passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name);
  bool isStore(const Expr *e) const;
  void keep(const Expr *value, const llvm::Twine &where);
  void markParam(const FunctionDecl *fn, unsigned index);
  void checkNews();
  void checkRuns();
  void checkIntervals();
  const FunctionDecl *reader(const FunctionDecl *fn) const;

  static const ClassDecl *storageClass(const ClassDecl *cls);
  static bool storageMethod(const CallExpr *c, llvm::StringRef name);
  static const Expr *bare(const Expr *e);
  static const FunctionDecl *callbackOf(const Expr *e);
  const FunctionDecl *programTarget(const CallExpr *c) const;

  Program &program_;
  std::vector<std::pair<const char *, const char *>> libraryRanges_;
  const FunctionDecl *current_ = nullptr;
  bool report_ = false;
  llvm::DenseSet<const VarSymbol *> storeVars_;
  std::set<std::pair<const FunctionDecl *, unsigned>> storeParams_;
  llvm::DenseSet<const FunctionDecl *> returnsStore_;
  llvm::DenseSet<const FunctionDecl *> walked_;
  llvm::DenseSet<const char *> reported_;
  llvm::DenseMap<const FunctionDecl *, std::vector<const FunctionDecl *>> callees_;
  llvm::DenseSet<const FunctionDecl *> readsDirectly_;
  std::vector<const CallExpr *> intervals_;
  std::vector<const CallExpr *> runs_;
  std::vector<const NewExpr *> news_;
  llvm::DenseSet<const Expr *> allowedNews_;
};

}
