#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/objects/dict_checker/dict_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class DictChecker {
public:
  using ResultRecordKeyChecked = Result<Expr *, DictCheckerCheckDictInError>;
  using ResultDictIndexedValue = Result<Expr *, DictCheckerCheckDictIndexError>;
  using ResultDictMemberAccess = Result<Expr *, DictCheckerCheckDictMemberError>;
  using ResultRecordKeyDeleted = Result<Expr *, DictCheckerCheckDictDeleteError>;
  using ResultKeyTypeIsAllowed = Result<const Type *, DictCheckerCheckKeyTypeError>;
  using ResultRecordTargetType = Result<const Type *, DictCheckerCheckDictTargetError>;
  using ResultDictTypeResolved = Result<const Type *, DictCheckerResolveDictTypeError>;

  explicit DictChecker(Checker &owner);

  Expr *iterableOf(Expr *expr);
  Expr *checkRecordKey(Expr *key);
  Expr *recordAssign(AssignExpr *assign);
  ResultRecordKeyChecked checkDictIn(BinaryExpr *bin);
  ResultKeyTypeIsAllowed checkKeyType(const Type *type);
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  ResultDictIndexedValue checkDictIndex(IndexExpr *index);
  ResultDictMemberAccess checkDictMember(MemberExpr *member);
  ResultDictTypeResolved resolveDictType(const TypeRef &ref);
  static bool keyArgument(DictCheckerResolveDictTypeErrorCode code);
  ResultRecordTargetType checkDictTarget(Expr *target, bool compound);
  Expr *checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index);
  ResultRecordKeyDeleted checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr,
                                         llvm::StringRef name);
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);

private:
  using ResultTextKeyForRecord = Result<const Type *, DictCheckerStringKeyError>;

  static std::u16string toUtf16(llvm::StringRef text);
  static ResultTextKeyForRecord stringKey(const Type *type);

  Checker &checker;
};

}
