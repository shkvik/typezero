#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class IntlChecker;
class TypeResolver;
class TypeRelations;
class ArgListChecker;
class WeakRefChecker;
class IterableChecker;

class DictChecker {
public:
  using ResultRecordKeyChecked = Result<Expr *, DictCheckerCheckDictInError>;
  using ResultDictIndexedValue = Result<Expr *, DictCheckerCheckDictIndexError>;
  using ResultDictMemberAccess = Result<Expr *, DictCheckerCheckDictMemberError>;
  using ResultRecordKeyDeleted = Result<Expr *, DictCheckerCheckDictDeleteError>;
  using ResultKeyTypeIsAllowed = Result<const Type *, DictCheckerCheckKeyTypeError>;
  using ResultRecordTargetType = Result<const Type *, DictCheckerCheckDictTargetError>;
  using ResultDictTypeResolved = Result<const Type *, DictCheckerResolveDictTypeError>;

  DictChecker(SemanticState &owner, SemanticParts &all);

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
  ResultDictTypeResolved resolveCollectionType(const TypeRef &ref, Collection collection);
  static ResultDictTypeResolved recordType(const Type *key, const Type *value);
  static bool isLiteralKeys(const Type *key);
  static const Type *literalKeysObject(const Type *key, const Type *value);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  IntlChecker *intlChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
  WeakRefChecker *weakRefChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
};

}
