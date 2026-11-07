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


namespace tz {

MemberLowerer::MemberLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *MemberLowerer::emitMember(MemberExpr *member) {
  if (member->method()) {
    return this->lowering.classLowerer().emitBoundMethod(member);
  }
  if (member->object->type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonMember(member);
  }
  const Type *objectType = member->object->type;
  if (objectType->isBytes() ||
      (objectType->isUnion() &&
       llvm::all_of(objectType->members(), [](const Type *m) { return m->isBytes(); }))) {
    return this->lowering.bytesLowerer().emitBytesMember(member);
  }
  if (member->object->type->isObject() || member->object->type->isUnion()) {
    return this->lowering.objectLowerer().emitFieldRead(member);
  }
  if (member->object->type->isResult()) {
    return this->lowering.resultLowerer().emitResultMember(member);
  }
  Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
  llvm::Value *length = object.type->isArray()
                            ? this->lowering.arrayLowerer().arrayLength(object.value)
                            : this->lowering.stringLowerer().stringLength(object.value);
  this->lowering.refCounts().dropOperand(object);

  return length;
}

}
