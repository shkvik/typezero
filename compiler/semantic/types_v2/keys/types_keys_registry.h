#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/keys/types_keys_registry_errors.h"

namespace tz {

class TypesKeysRegistry {
public:
  explicit TypesKeysRegistry(SemanticState &owner);

  void evalKeyOf(const EvalKeyOf &task);
  void evalSpread(const EvalSpread &task);
  void startKeyOf(const StartKeyOf &task);
  void evalIndexed(const EvalIndexed &task);
  void startIndexed(const StartIndexed &task);
  void afterKeyOfEval(const AfterKeyOfEval &task);
  void afterIndexedEval(const AfterIndexedEval &task);
  void afterKeyOfOperand(const AfterKeyOfOperand &task);
  void afterIndexedOperands(const AfterIndexedOperands &task);

private:
  bool isLengthKey(const Type *key) const;
  bool isNumericKey(const Type *key) const;
  bool isPublicKey(const ClassDecl *classDecl, llvm::StringRef name) const;


  const Type *keysOf(const Type *type) const;
  const Type *ownKeys(const Type *type) const;
  const Type *lengthType(const Type *object) const;
  const Type *memberType(const Type *object, const Type *key) const;
  const Type *tupleFieldAt(const Type *tuple, double position) const;
  const Type *tupleMemberType(const Type *tuple, const Type *key) const;
  const Type *objectMemberType(const Type *object, const Type *key) const;


  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  std::vector<const Type *> sharedKeys(llvm::ArrayRef<const Type *> common, const Type *keys,
                                       bool isFirst) const;

  SemanticState &state;
};

}
