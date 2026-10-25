#pragma once

#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/lowering/lowering.h"
#include <vector>

namespace tz {

// TzBytes; an ArrayBuffer has data and length at the same places.
enum BytesField : unsigned { kRefCount, kLayout, kData, kLength, kOffset, kBuffer };

// The value of an int literal; whether an argument is a literal.
int64_t constantOf(const Expr *e);
bool isConstant(const Expr *e);
// The struct of TzBytes.
llvm::StructType *bytesStruct(llvm::LLVMContext &ctx);
bool isSignedKind(Bytes kind);

// A call of binary data being lowered (BytesCallLowerer): the call, what the cases of its group
// use of it, and the steps they share. A typed array, a Buffer or a DataView is a pointer to a
// TzBytes, an ArrayBuffer to a TzArrayBuffer (runtime/library/bytes/bytes/bytes.h); both have
// their data pointer and their length at the same offsets. The code reads and writes elements
// itself.
class BytesCallSite {
public:
  BytesCallSite(Lowering &lowering, CallExpr *call);

protected:
  // A function of the runtime.
  llvm::FunctionCallee fn(llvm::StringRef name, llvm::Type *ret,
                          llvm::ArrayRef<llvm::Type *> params);
  llvm::ConstantInt *kindOf(const Type *t);
  // The receiver of a method, held while the arguments from `from` on are evaluated if they (or
  // an encoding among them) may throw; takeOperand once they are.
  Lowering::Operand receiver(size_t from);
  // A method that gives the receiver back hands over its reference.
  llvm::Value *returnSelf(Lowering::Operand &self);
  // Optional positions from args[from] on: their values (0 when left out) and a mask of those
  // given, from bit 0 on.
  llvm::Value *positions(size_t from, size_t count, std::vector<llvm::Value *> &values);
  // An encoding: a constant Checker found, or a string checked now.
  llvm::Value *encoding(Expr *arg);

  Lowering &lowering_;
  CallExpr *call;
  MemberExpr *member;
  const Type *type;
  bool throws;
  llvm::Type *voidTy, *i32, *i64, *f64, *ptr;
};

} // namespace tz
