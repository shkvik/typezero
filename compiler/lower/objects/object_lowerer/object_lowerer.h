#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ObjectLowerer {
public:
  explicit ObjectLowerer(Lowering &lowering);

  llvm::Value *emitObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitSpreadLiteral(ObjectLitExpr *lit);
  llvm::Value *emitTupleLiteral(ArrayLitExpr *lit);
  void emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                       const Type *element);
  llvm::Value *emitObjectCall(CallExpr *call);
  llvm::Value *emitFieldRead(MemberExpr *member);
  llvm::Value *emitFieldAssign(AssignExpr *assign, bool needResult);
  llvm::Value *keyedFieldAddress(llvm::Value *object, const Type *type, llvm::Value *key,
                                 const Type *keyType);

private:
  Lowering &lowering_;
};

}
