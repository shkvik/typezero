#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// unknown, classes as values and join of tuples.
class UnknownLowerer {
public:
  explicit UnknownLowerer(CodeGen &codegen);

  // unknown, object and classes as values (codegen_unknown.cpp). An unknown
  // is a pointer, null for undefined: an object of the language as it is,
  // any other value in a box of the runtime (TzBox,
  // runtime/core/unknown/unknown.c). A class as a value is a constant object
  // (TzClass) made when first used.
  llvm::FunctionCallee unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  // An owned value of `from` as one of `to`, one of them an unknown.
  llvm::Value *emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to);
  // An owned value of `type` (not an object of the language) in a new box.
  llvm::Value *boxValue(llvm::Value *value, const Type *type);
  llvm::Value *emitUnknownTypeOf(Expr *operand);
  llvm::Value *emitUnknownIsArray(Expr *arg);
  llvm::Constant *classObject(const ClassDecl *cls);
  // The address of a static field in the object of its class (when the
  // class is used as a value: ClassDecl::usedAsValue).
  llvm::Constant *staticFieldAddress(const ClassDecl *cls, const VarSymbol *var);
  // `t.join(sep)` of a tuple.
  llvm::Value *emitTupleJoin(CallExpr *call);

private:
  CodeGen &codegen_;
  llvm::DenseMap<const ClassDecl *, llvm::StructType *> classObjectTypes_;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classObjects_;
};

} // namespace tz
