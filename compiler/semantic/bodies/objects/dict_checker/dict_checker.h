#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_types.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
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
  ResultRecordKeyChecked checkDictIn(BinaryExpr *binary);
  ResultKeyTypeIsAllowed checkKeyType(const Type *type);
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  ResultDictIndexedValue checkDictIndex(IndexExpr *index);
  ResultDictMemberAccess checkDictMember(MemberExpr *member);
  ResultDictTypeResolved resolveDictType(const TypeRef &ref);
  bool keyArgument(DictCheckerResolveDictTypeErrorCode code) const;
  ResultRecordTargetType checkDictTarget(Expr *target, AssignForm form);
  Expr *checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index);
  ResultRecordKeyDeleted checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr,
                                         llvm::StringRef name);
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);

private:
  using ResultTextKeyForRecord = Result<const Type *, DictCheckerStringKeyError>;

  ResultTextKeyForRecord stringKey(const Type *type) const;
  ResultDictTypeResolved resolveCollectionType(const TypeRef &ref, Collection collection);
  ResultDictTypeResolved recordType(const Type *key, const Type *value) const;
  bool isLiteralKeys(const Type *key) const;
  const Type *literalKeysObject(const Type *key, const Type *value) const;

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  IntlChecker *intlChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
  WeakRefChecker *weakRefChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
};

}
