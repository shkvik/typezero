#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"

namespace tz::parallel {

class CodeWalk {
public:
  explicit CodeWalk(Analysis &analysis);

  int discover(const FunctionDecl *fn);
  void walk(int fi);
  void stmt(const Stmt *s);
  void anchored(const Stmt *s);
  void declarator(const VarDeclarator &d);
  void forOf(const ForOfStmt *f, llvm::StringRef label);
  void iterate(const Expr *iterable, const Node *site);
  void loop(llvm::StringRef label, llvm::function_ref<void()> head, llvm::function_ref<void()> body,
            llvm::function_ref<void()> step, bool headFirst);
  void joinPoint();
  void moveTo(unsigned seg);
  unsigned segment(const Node *at, SegKind kind, llvm::SMLoc loc);
  CallRec &record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc);
  void noteClass(const ClassDecl *cls);
  void touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at,
             Res res = Res::Stdout);
  void var(const VarSymbol *v, bool write, llvm::SMLoc at);

private:
  Analysis &analysis_;
};

}
