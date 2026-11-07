#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnknownLowerer {
public:
  explicit UnknownLowerer(Lowering &owner);

  llvm::FunctionCallee unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *boxValue(llvm::Value *value, const Type *type);
  llvm::Value *emitUnknownTypeOf(Expr *operand);
  llvm::Value *typeOfText(llvm::Value *code);
  llvm::Value *emitUnknownIsJson(Expr *arg);
  llvm::Value *emitUnknownIsArray(Expr *arg);
  llvm::Constant *classObject(const ClassDecl *cls);
  llvm::Constant *staticFieldAddress(const ClassDecl *cls, const VarSymbol *var);
  llvm::Value *emitTupleJoin(CallExpr *call);

private:
  static bool hasCall(const Expr *expr);
  static bool isObjectHeaded(const Type *type);

  Lowering &lowering;
  llvm::DenseMap<const ClassDecl *, llvm::StructType *> classObjectTypes;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classObjects;
};

}
