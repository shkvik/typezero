#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Object literals, tuples, fields and Object.keys.
class ObjectLowerer {
public:
  explicit ObjectLowerer(CodeGen &codegen);

  llvm::Value *emitObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitSpreadLiteral(ObjectLitExpr *lit);
  // `["Ann", 30]` as a tuple.
  llvm::Value *emitTupleLiteral(ArrayLitExpr *lit);
  // Appends the elements of the borrowed tuple `tuple` to `array`.
  void emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                       const Type *element);
  llvm::Value *emitObjectCall(CallExpr *call);
  llvm::Value *emitFieldRead(MemberExpr *member);
  llvm::Value *emitFieldAssign(AssignExpr *assign, bool needResult);
  // `o[k]` of a key of literals (IndexExpr::keyed): the address of the field
  // the atom `key` names.
  llvm::Value *keyedFieldAddress(llvm::Value *object, const Type *type, llvm::Value *key,
                                 const Type *keyType);

private:
  CodeGen &codegen_;
};

} // namespace tz
