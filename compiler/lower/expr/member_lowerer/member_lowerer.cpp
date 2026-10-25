#include "compiler/lower/expr/member_lowerer/member_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

MemberLowerer::MemberLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *MemberLowerer::emitMember(MemberExpr *member) {
  if (member->method) {
    return lowering_.classLowerer().emitBoundMethod(member);
  }
  if (member->object->type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonMember(member);
  }
  // A union of binary data too, whose members keep the length alike.
  const Type *objectType = member->object->type;
  if (objectType->isBytes() ||
      (objectType->isUnion() &&
       llvm::all_of(objectType->members(), [](const Type *m) { return m->isBytes(); }))) {
    return lowering_.bytesLowerer().emitBytesMember(member);
  }
  if (member->object->type->isObject() || member->object->type->isUnion()) {
    return lowering_.objectLowerer().emitFieldRead(member);
  }
  if (member->object->type->isResult()) {
    return lowering_.resultLowerer().emitResultMember(member);
  }
  // Otherwise Checker allows only `.length` of a string or an array.
  Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *length = object.type->isArray()
                            ? lowering_.arrayLowerer().arrayLength(object.value)
                            : lowering_.stringLowerer().stringLength(object.value);
  lowering_.refCounts().dropOperand(object);

  return lowering_.builder_.CreateTrunc(length, lowering_.builder_.getInt32Ty(), "length");
}

} // namespace tz
