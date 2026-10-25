#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

UnionMemberLowerer::UnionMemberLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *UnionMemberLowerer::unionFieldAddress(llvm::Value *object, const Type *type,
                                        llvm::StringRef name) {
  llvm::ArrayRef<const Type *> members = type->members();
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  auto offsetIn = [&](const Type *m) -> int64_t {
    if (codegen_.inheritLowerer().dispatchedMember(m, name)) {
      return -1;
    }
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(m);

    return int64_t(
        data.getStructLayout(layout.type)->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(m, name)]));
  };
  int64_t offset = offsetIn(members.front());
  if (offset >= 0 && llvm::all_of(members, [&](const Type *m) { return offsetIn(m) == offset; })) {
    return codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, uint64_t(offset),
                                               name + ".addr");
  }
  // Each member puts it elsewhere: chosen by the class number of the object.
  llvm::Value *id = codegen_.unionLowerer().objectClassId(object);
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("union.field.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (size_t i = 0; i < members.size(); ++i) {
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < members.size()) {
      llvm::BasicBlock *block = codegen_.functionFrame().newBlock("union.field");
      next = codegen_.functionFrame().newBlock("union.field.next");
      codegen_.builder_.CreateCondBr(codegen_.unionLowerer().emitIsOneOf(id, members[i]), block, next);
      codegen_.functionFrame().switchTo(block);
    }
    incoming.push_back({codegen_.objectLayouts().fieldAddress(object, members[i], name), codegen_.builder_.GetInsertBlock()});
    codegen_.builder_.CreateBr(endBlock);
    if (next) {
      codegen_.functionFrame().switchTo(next);
    }
  }
  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getPtrTy(), unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

llvm::Value *UnionMemberLowerer::emitUnionFieldRead(MemberExpr *member) {
  const Type *type = member->object->type;
  CodeGen::Operand object = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::ArrayRef<const Type *> members = type->members();
  // A field of one type in every member: one load, from one offset if they
  // agree on it.
  const Type *fieldType = members.front()->field(member->name)->type;
  bool same = llvm::all_of(
      members, [&](const Type *m) { return m->field(member->name)->type == fieldType; });
  llvm::Value *result;
  if (same) {
    llvm::Value *address = unionFieldAddress(object.value, type, member->name);
    result = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(fieldType), address, member->name);
    codegen_.refCounts().retainValue(result, fieldType);
    result = codegen_.exprLowerer().emitConversion(result, fieldType, member->type);
  } else {
    // Each member reads its own, chosen by the class number of the object.
    llvm::Value *id = codegen_.unionLowerer().objectClassId(object.value);
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("union.field.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (size_t i = 0; i < members.size(); ++i) {
      const Type *m = members[i];
      llvm::BasicBlock *next = nullptr;
      if (i + 1 < members.size()) {
        llvm::BasicBlock *read = codegen_.functionFrame().newBlock("union.field");
        next = codegen_.functionFrame().newBlock("union.field.next");
        codegen_.builder_.CreateCondBr(codegen_.unionLowerer().emitIsOneOf(id, m), read, next);
        codegen_.functionFrame().switchTo(read);
      }
      const Type *own = m->field(member->name)->type;
      llvm::Value *value = codegen_.builder_.CreateLoad(
          codegen_.moduleBuilder().lower(own), codegen_.objectLayouts().fieldAddress(object.value, m, member->name), member->name);
      codegen_.refCounts().retainValue(value, own);
      incoming.push_back({codegen_.exprLowerer().emitConversion(value, own, member->type), codegen_.builder_.GetInsertBlock()});
      codegen_.builder_.CreateBr(endBlock);
      if (next) {
        codegen_.functionFrame().switchTo(next);
      }
    }
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(member->type), unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  codegen_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *UnionMemberLowerer::emitObjectIn(BinaryExpr *bin) {
  // `"radius" in s`: whether the object is of a member with that field.
  codegen_.stmtLowerer().emitDiscarded(bin->lhs);
  const Type *type = bin->rhs->type;
  CodeGen::Operand value = codegen_.refCounts().emitOperand(bin->rhs, /*canBorrow=*/true);
  std::string name;
  const Expr *key = bin->lhs;
  while (auto *conv = dyn_cast<CastExpr>(key)) {
    key = conv->operand;
  }
  if (auto *text = dyn_cast<StringLitExpr>(key)) {
    llvm::convertUTF16ToUTF8String(
        llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text->value.data()),
                                    text->value.size()),
        name);
  }
  llvm::Value *object = value.value;
  llvm::Value *result;
  const Type *objects = type->nonOptional();
  llvm::Value *present = type->isOptional() ? codegen_.optionalLowerer().optionalHas(object, type) : codegen_.builder_.getTrue();
  if (type->isOptional()) {
    object = codegen_.optionalLowerer().optionalValue(object, type);
  }
  std::vector<const Type *> with;
  if (objects->isTagged()) {
    // An object only in its slot.
    llvm::Value *isObject =
        codegen_.builder_.CreateICmpEQ(codegen_.unionLayout().unionTag(object), codegen_.builder_.getInt32(unsigned(UnionSlot::Object)));
    present = codegen_.builder_.CreateAnd(present, isObject);
    objects = objects->slotPart(UnionSlot::Object);
    object = codegen_.builder_.CreateSelect(isObject,
                                   codegen_.unionLayout().taggedPart(object, value.type->nonOptional(), UnionSlot::Object),
                                   llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()));
  }
  for (const Type *m :
       objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(objects)) {
    if (m->isObject() && m->field(name)) {
      with.push_back(m);
    }
  }
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *testBlock = codegen_.functionFrame().newBlock("in.test");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("in.end");
  codegen_.builder_.CreateCondBr(present, testBlock, endBlock);
  codegen_.functionFrame().switchTo(testBlock);
  llvm::Value *matches = codegen_.unionLowerer().emitIsOneOf(codegen_.unionLowerer().objectClassId(object), with);
  llvm::BasicBlock *testEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2);
  phi->addIncoming(codegen_.builder_.getFalse(), before);
  phi->addIncoming(matches, testEnd);
  result = phi;
  codegen_.refCounts().dropOperand(value);

  return result;
}

llvm::Value *UnionMemberLowerer::emitUnionMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *fnType = member->type;
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, AssignLowerer::cannotAssign);
  CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, borrow);
  codegen_.throwLowerer().holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  codegen_.throwLowerer().takeOperand(self);
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i) {
    values.push_back(codegen_.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  // The member the object is of tells which runs; each gets the arguments.
  llvm::Value *id = codegen_.unionLowerer().objectClassId(self.value);
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("union.call.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  bool throws = false;
  for (size_t i = 0; i < call->unionMethods.size(); ++i) {
    const CallExpr::UnionMethod &m = call->unionMethods[i];
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < call->unionMethods.size()) {
      llvm::BasicBlock *block = codegen_.functionFrame().newBlock("union.call");
      next = codegen_.functionFrame().newBlock("union.call.next");
      codegen_.builder_.CreateCondBr(codegen_.unionLowerer().emitIsOneOf(id, m.member), block, next);
      codegen_.functionFrame().switchTo(block);
    }
    llvm::Value *result;
    if (m.method) {
      std::vector<llvm::Value *> args{self.value};
      args.insert(args.end(), values.begin(), values.end());
      result = m.virtualCall
                   ? codegen_.builder_.CreateCall(codegen_.closureLowerer().closureFnType(ClosureLowerer::functionTypeOf(m.method)),
                                         codegen_.inheritLowerer().vtableEntry(self.value, m.method->decl->slot, false), args)
                   : codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(m.method->decl), args);
      throws |= codegen_.mayThrow().callMayThrow(m.method->decl, m.virtualCall);
    } else {
      // The function in the field, borrowed from the object.
      llvm::Value *fn =
          codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.objectLayouts().fieldAddress(self.value, m.member, member->name));
      result = codegen_.closureLowerer().callFunctionValue(fn, fnType, values);
      throws |= codegen_.typeThrows(fnType);
    }
    if (result && !result->getType()->isVoidTy()) {
      incoming.push_back({result, codegen_.builder_.GetInsertBlock()});
    }
    codegen_.builder_.CreateBr(endBlock);
    if (next) {
      codegen_.functionFrame().switchTo(next);
    }
  }
  codegen_.functionFrame().switchTo(endBlock);
  llvm::Value *result = nullptr;
  if (!incoming.empty()) {
    llvm::PHINode *phi =
        codegen_.builder_.CreatePHI(incoming.front().first->getType(), unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  codegen_.refCounts().dropOperand(self);
  codegen_.emitThrowCheckIf(throws && call != codegen_.returnCall_);

  return result;
}

} // namespace tz
