#pragma once

#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/lowering/lowering.h"
#include <vector>

namespace tz {

enum BytesField : unsigned { kRefCount, kLayout, kData, kLength, kOffset, kBuffer };

class BytesCallSite {
public:
  BytesCallSite(Lowering &lowering, CallExpr *call);

protected:
  static int64_t constantOf(const Expr *e);

  llvm::FunctionCallee fn(llvm::StringRef name, llvm::Type *ret,
                          llvm::ArrayRef<llvm::Type *> params);
  llvm::ConstantInt *kindOf(const Type *t);
  Lowering::Operand receiver(size_t from);
  llvm::Value *returnSelf(Lowering::Operand &self);
  llvm::Value *positions(size_t from, size_t count, std::vector<llvm::Value *> &values);
  llvm::Value *encoding(Expr *arg);

  Lowering &lowering;
  CallExpr *call;
  MemberExpr *member;
  const Type *type;
  llvm::Type *voidTy, *i32, *i64, *f64, *ptr;
};

}
