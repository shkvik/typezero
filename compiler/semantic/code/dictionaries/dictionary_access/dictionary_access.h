#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class DictionaryAccess {
public:
  explicit DictionaryAccess(SemanticState &owner);

  void checkDictIn(const CheckDictIn &task);
  void checkDictIndex(const CheckDictIndex &task);
  void finishRecordKey(const FinishRecordKey &task);
  void checkDictMember(const CheckDictMember &task);
  void checkDictTarget(const CheckDictTarget &task);
  void checkDictDelete(const CheckDictDelete &task);
  void finishDictTarget(const FinishDictTarget &task);
  void finishDictDelete(const FinishDictDelete &task);
  void checkRecordKeySlot(const CheckRecordKeySlot &task);
  void finishDictIndexRead(const FinishDictIndexRead &task);
  void finishNullishRecord(const FinishNullishRecord &task);
  void finishDictIndexError(const FinishDictIndexError &task);
  void afterNullishRecordKey(const AfterNullishRecordKey &task);
  void checkRecordNullishAssign(const CheckRecordNullishAssign &task);
  void afterNullishRecordObject(const AfterNullishRecordObject &task);

private:
  using ResultDictMemberAccess = Result<Expr *, CodeDictionariesRegistryCheckDictMemberError>;
  using ResultTextKeyForRecord = Result<const Type *, CodeDictionariesRegistryStringKeyError>;
  using ResultRecordKeyChecked = Result<const Type *, CodeDictionariesRegistryCheckDictInError>;
  using ResultDictIndexedValue = Result<const Type *, CodeDictionariesRegistryCheckDictIndexError>;
  using ResultRecordKeyDeleted = Result<const Type *, CodeDictionariesRegistryCheckDictDeleteError>;
  using ResultRecordTargetType = Result<const Type *, CodeDictionariesRegistryCheckDictTargetError>;

  void pushDictRemoval(UnaryExpr *unary, Expr *object, Expr *key);


  bool isMethodOn(const DictMethodInfo &method, const Type *type) const;


  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);


  std::string missingHint(const std::string &key, const Type *value) const;
  llvm::SMLoc inErrorLoc(const BinaryExpr *binary,
                         CodeDictionariesRegistryCheckDictInErrorCode code) const;


  ResultDictMemberAccess checkMemberAccess(MemberExpr *member);
  ResultTextKeyForRecord checkStringKey(const Type *type) const;
  ResultRecordKeyDeleted checkRecordDelete(const Expr *object) const;
  ResultRecordKeyChecked checkInOperands(const BinaryExpr *binary) const;
  ResultDictIndexedValue checkCollectionIndex(const IndexExpr *index) const;
  ResultRecordTargetType checkCollectionTarget(const IndexExpr *index, const MemberExpr *member,
                                               const Expr *object) const;
  ResultRecordTargetType checkRecordTarget(const IndexExpr *index, const MemberExpr *member,
                                           const Expr *object, AssignForm form) const;


  llvm::StringRef collectionName(const Type *type) const;

  SemanticState &state;
};

}
