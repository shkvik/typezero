#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
#include <map>
#include <set>

namespace tz {

std::string utf8Of(const std::u16string &text);

const Expr *bare(const Expr *e);

class OwnerScanner {
public:
  struct BackRef {
    const ClassDecl *owner;
    llvm::StringRef field;
    llvm::StringRef callback;
    const FunctionDecl *fn;
    const VarSymbol *self;
  };

  void function(const FunctionDecl *fn);

  void topLevel(const Stmt *s);

  std::vector<BackRef> proven(const std::function<bool(const ClassDecl *)> &escapes) const;

  const ClassDecl *madeBy(llvm::StringRef field) const;

private:
  enum class Use { Value, Receiver, Callee };
  struct Write {
    const ClassDecl *cls;
    const ClassDecl *made;
  };

  void stmt(const Stmt *stmt);

  void expr(const Expr *e, Use use);

  void readAll(llvm::StringRef name);

  void readFields(const Type *type);

  void wrote(const MemberExpr *target, const AssignExpr *assign);

  static bool mentions(const Expr *e, const VarSymbol *self);

  static void visitChildren(const Expr *e, const std::function<void(const Expr *)> &visit);

  llvm::DenseSet<const FunctionDecl *> scanned_;
  const FunctionDecl *current_ = nullptr;
  llvm::StringSet<> valueReads_, nonCallReads_, bad_;
  llvm::StringMap<std::vector<Write>> writes_;
  std::vector<BackRef> candidates_;
};

}
