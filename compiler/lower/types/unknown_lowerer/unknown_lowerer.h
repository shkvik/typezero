#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnknownLowerer {
public:
  explicit UnknownLowerer(Lowering &lowering);

  llvm::FunctionCallee unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *boxValue(llvm::Value *value, const Type *type);
  llvm::Value *emitUnknownTypeOf(Expr *operand);
  llvm::Value *emitUnknownIsArray(Expr *arg);
  llvm::Constant *classObject(const ClassDecl *cls);
  llvm::Constant *staticFieldAddress(const ClassDecl *cls, const VarSymbol *var);
  llvm::Value *emitTupleJoin(CallExpr *call);

private:
  Lowering &lowering_;
  llvm::DenseMap<const ClassDecl *, llvm::StructType *> classObjectTypes_;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classObjects_;
};

}
