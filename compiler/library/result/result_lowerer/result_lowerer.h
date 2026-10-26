#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ResultLowerer {
public:
  explicit ResultLowerer(Lowering &lowering);

  Lowering::ResultLayout resultLayout(const Type *type);
  llvm::Type *lowerHalf(const Type *half);
  llvm::Constant *resultDescriptor(const Type *type);
  llvm::Value *resultIsOk(llvm::Value *result, const Type *type);
  llvm::Value *resultHalf(llvm::Value *result, const Type *type, bool error);
  llvm::Value *resultSite(llvm::Value *result, const Type *type);
  llvm::Value *makeResult(const Type *type, llvm::Value *ok, llvm::Value *value, llvm::Value *error,
                          llvm::Value *site = nullptr);
  llvm::Value *convertHalf(llvm::Value *half, const Type *from, const Type *to);
  llvm::Value *callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                            const Type *halfType);
  llvm::Value *emitResultCall(CallExpr *call);
  llvm::Value *emitResultMethod(CallExpr *call);
  llvm::Value *emitResultMember(MemberExpr *member);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, Lowering::ResultLayout> resultLayouts_;
};

}
