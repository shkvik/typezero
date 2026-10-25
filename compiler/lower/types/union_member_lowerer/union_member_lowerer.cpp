#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

UnionMemberLowerer::UnionMemberLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *UnionMemberLowerer::unionFieldAddress(llvm::Value *object, const Type *type,
                                                   llvm::StringRef name) {
  llvm::ArrayRef<const Type *> members = type->members();
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  auto offsetIn = [&](const Type *m) -> int64_t {
    if (lowering_.inheritLowerer().dispatchedMember(m, name)) {
      return -1;
    }
    const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(m);

    return int64_t(data.getStructLayout(layout.type)
                       ->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(m, name)]));
  };
  int64_t offset = offsetIn(members.front());
  if (offset >= 0 && llvm::all_of(members, [&](const Type *m) { return offsetIn(m) == offset; })) {
    return lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object,
                                                         uint64_t(offset), name + ".addr");
  }
  // Each member puts it elsewhere: chosen by the class number of the object.
  llvm::Value *id = lowering_.unionLowerer().objectClassId(object);
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("union.field.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (size_t i = 0; i < members.size(); ++i) {
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < members.size()) {
      llvm::BasicBlock *block = lowering_.functionFrame().newBlock("union.field");
      next = lowering_.functionFrame().newBlock("union.field.next");
      lowering_.builder_.CreateCondBr(lowering_.unionLowerer().emitIsOneOf(id, members[i]), block,
                                      next);
      lowering_.functionFrame().switchTo(block);
    }
    incoming.push_back({lowering_.objectLayouts().fieldAddress(object, members[i], name),
                        lowering_.builder_.GetInsertBlock()});
    lowering_.builder_.CreateBr(endBlock);
    if (next) {
      lowering_.functionFrame().switchTo(next);
    }
  }
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi =
      lowering_.builder_.CreatePHI(lowering_.builder_.getPtrTy(), unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

llvm::Value *UnionMemberLowerer::emitUnionFieldRead(MemberExpr *member) {
  const Type *type = member->object->type;
  Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::ArrayRef<const Type *> members = type->members();
  // A field of one type in every member: one load, from one offset if they
  // agree on it.
  const Type *fieldType = members.front()->field(member->name)->type;
  bool same = llvm::all_of(
      members, [&](const Type *m) { return m->field(member->name)->type == fieldType; });
  llvm::Value *result;
  if (same) {
    llvm::Value *address = unionFieldAddress(object.value, type, member->name);
    result = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(fieldType), address,
                                           member->name);
    lowering_.refCounts().retainValue(result, fieldType);
    result = lowering_.exprLowerer().emitConversion(result, fieldType, member->type);
  } else {
    // Each member reads its own, chosen by the class number of the object.
    llvm::Value *id = lowering_.unionLowerer().objectClassId(object.value);
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("union.field.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (size_t i = 0; i < members.size(); ++i) {
      const Type *m = members[i];
      llvm::BasicBlock *next = nullptr;
      if (i + 1 < members.size()) {
        llvm::BasicBlock *read = lowering_.functionFrame().newBlock("union.field");
        next = lowering_.functionFrame().newBlock("union.field.next");
        lowering_.builder_.CreateCondBr(lowering_.unionLowerer().emitIsOneOf(id, m), read, next);
        lowering_.functionFrame().switchTo(read);
      }
      const Type *own = m->field(member->name)->type;
      llvm::Value *value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(own),
          lowering_.objectLayouts().fieldAddress(object.value, m, member->name), member->name);
      lowering_.refCounts().retainValue(value, own);
      incoming.push_back({lowering_.exprLowerer().emitConversion(value, own, member->type),
                          lowering_.builder_.GetInsertBlock()});
      lowering_.builder_.CreateBr(endBlock);
      if (next) {
        lowering_.functionFrame().switchTo(next);
      }
    }
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(member->type),
                                                      unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  lowering_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *UnionMemberLowerer::emitObjectIn(BinaryExpr *bin) {
  // `"radius" in s`: whether the object is of a member with that field.
  lowering_.stmtLowerer().emitDiscarded(bin->lhs);
  const Type *type = bin->rhs->type;
  Lowering::Operand value = lowering_.refCounts().emitOperand(bin->rhs, /*canBorrow=*/true);
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
  llvm::Value *present = type->isOptional() ? lowering_.optionalLowerer().optionalHas(object, type)
                                            : lowering_.builder_.getTrue();
  if (type->isOptional()) {
    object = lowering_.optionalLowerer().optionalValue(object, type);
  }
  std::vector<const Type *> with;
  if (objects->isTagged()) {
    // An object only in its slot.
    llvm::Value *isObject =
        lowering_.builder_.CreateICmpEQ(lowering_.unionLayout().unionTag(object),
                                        lowering_.builder_.getInt32(unsigned(UnionSlot::Object)));
    present = lowering_.builder_.CreateAnd(present, isObject);
    objects = objects->slotPart(UnionSlot::Object);
    object = lowering_.builder_.CreateSelect(
        isObject,
        lowering_.unionLayout().taggedPart(object, value.type->nonOptional(), UnionSlot::Object),
        llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()));
  }
  for (const Type *m :
       objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(objects)) {
    if (m->isObject() && m->field(name)) {
      with.push_back(m);
    }
  }
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *testBlock = lowering_.functionFrame().newBlock("in.test");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("in.end");
  lowering_.builder_.CreateCondBr(present, testBlock, endBlock);
  lowering_.functionFrame().switchTo(testBlock);
  llvm::Value *matches =
      lowering_.unionLowerer().emitIsOneOf(lowering_.unionLowerer().objectClassId(object), with);
  llvm::BasicBlock *testEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2);
  phi->addIncoming(lowering_.builder_.getFalse(), before);
  phi->addIncoming(matches, testEnd);
  result = phi;
  lowering_.refCounts().dropOperand(value);

  return result;
}

llvm::Value *UnionMemberLowerer::emitUnionMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *fnType = member->type;
  bool borrow =
      staysDuringCall(member->object) && llvm::all_of(call->args, AssignLowerer::cannotAssign);
  Lowering::Operand self = lowering_.refCounts().emitOperand(member->object, borrow);
  lowering_.throwLowerer().holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) {
                                         return lowering_.mayThrow().exprThrows(arg);
                                       }));
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  lowering_.throwLowerer().takeOperand(self);
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i) {
    values.push_back(lowering_.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  // The member the object is of tells which runs; each gets the arguments.
  llvm::Value *id = lowering_.unionLowerer().objectClassId(self.value);
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("union.call.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  bool throws = false;
  for (size_t i = 0; i < call->unionMethods.size(); ++i) {
    const CallExpr::UnionMethod &m = call->unionMethods[i];
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < call->unionMethods.size()) {
      llvm::BasicBlock *block = lowering_.functionFrame().newBlock("union.call");
      next = lowering_.functionFrame().newBlock("union.call.next");
      lowering_.builder_.CreateCondBr(lowering_.unionLowerer().emitIsOneOf(id, m.member), block,
                                      next);
      lowering_.functionFrame().switchTo(block);
    }
    llvm::Value *result;
    if (m.method) {
      std::vector<llvm::Value *> args{self.value};
      args.insert(args.end(), values.begin(), values.end());
      result = m.virtualCall
                   ? lowering_.builder_.CreateCall(lowering_.closureLowerer().closureFnType(
                                                       ClosureLowerer::functionTypeOf(m.method)),
                                                   lowering_.inheritLowerer().vtableEntry(
                                                       self.value, m.method->decl->slot, false),
                                                   args)
                   : lowering_.builder_.CreateCall(
                         lowering_.classDescriptors().methodCode(m.method->decl), args);
      throws |= lowering_.mayThrow().callMayThrow(m.method->decl, m.virtualCall);
    } else {
      // The function in the field, borrowed from the object.
      llvm::Value *fn = lowering_.builder_.CreateLoad(
          lowering_.fnTy_,
          lowering_.objectLayouts().fieldAddress(self.value, m.member, member->name));
      result = lowering_.closureLowerer().callFunctionValue(fn, fnType, values);
      throws |= lowering_.typeThrows(fnType);
    }
    if (result && !result->getType()->isVoidTy()) {
      incoming.push_back({result, lowering_.builder_.GetInsertBlock()});
    }
    lowering_.builder_.CreateBr(endBlock);
    if (next) {
      lowering_.functionFrame().switchTo(next);
    }
  }
  lowering_.functionFrame().switchTo(endBlock);
  llvm::Value *result = nullptr;
  if (!incoming.empty()) {
    llvm::PHINode *phi =
        lowering_.builder_.CreatePHI(incoming.front().first->getType(), unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  lowering_.refCounts().dropOperand(self);
  lowering_.emitThrowCheckIf(throws && call != lowering_.returnCall_);

  return result;
}

} // namespace tz
