#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DictChecker {
public:
  explicit DictChecker(Checker &checker);

  const Type *resolveDictType(const TypeRef &ref);
  bool checkKeyType(const Type *type, llvm::SMLoc loc);
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);
  Expr *iterableOf(Expr *expr);
  Expr *checkRecordKey(Expr *key);
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  Expr *checkDictMember(MemberExpr *member);
  Expr *checkDictIndex(IndexExpr *index);
  const Type *checkDictTarget(Expr *target, bool compound);
  Expr *recordAssign(AssignExpr *assign);
  Expr *checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index);
  Expr *checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name);
  Expr *checkDictIn(BinaryExpr *bin);

private:
  Checker &checker_;
};

}
