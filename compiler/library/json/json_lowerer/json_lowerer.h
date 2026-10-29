#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class JsonLowerer {
public:
  explicit JsonLowerer(Lowering &lowering);

  llvm::FunctionCallee jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *jsonPresent(llvm::Value *value);
  llvm::Value *emitJsonNull();
  llvm::Value *emitJsonMember(MemberExpr *member);
  llvm::Value *emitJsonIndex(IndexExpr *index);
  llvm::Value *emitJsonAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitJsonConversion(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *emitJsonEquality(BinaryExpr *bin);
  llvm::Value *emitJsonNullish(BinaryExpr *bin);
  llvm::Value *emitJsonChainLink(CastExpr *link);
  llvm::Value *emitJsonTypeOf(Expr *operand);
  llvm::Value *emitJsonCall(CallExpr *call);
  llvm::Value *emitJsonObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitJsonObjectCall(CallExpr *call);
  void useErrors(const Type *errorType);

private:
  Lowering &lowering_;
};

}
