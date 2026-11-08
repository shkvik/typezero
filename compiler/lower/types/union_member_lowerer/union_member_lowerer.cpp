#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
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
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

UnionMemberLowerer::UnionMemberLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *UnionMemberLowerer::unionFieldAddress(llvm::Value *object, const Type *type,
                                                   llvm::StringRef name) {
  llvm::ArrayRef<const Type *> members = type->members();
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  auto offsetIn = [&](const Type *m) -> int64_t {
    if (this->lowering.inheritLowerer().dispatchedMember(m, name)) {
      return -1;
    }
    const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(m);

    return int64_t(data.getStructLayout(layout.type)
                       ->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(m, name)]));
  };
  int64_t offset = offsetIn(members.front());
  if (offset >= 0 && llvm::all_of(members, [&](const Type *m) { return offsetIn(m) == offset; })) {
    return this->lowering.builder.CreateConstInBoundsGEP1_64(
        this->lowering.builder.getInt8Ty(), object, uint64_t(offset), name + ".addr");
  }
  llvm::Value *id = this->lowering.unionLowerer().objectClassId(object);
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("union.field.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (size_t i = 0; i < members.size(); ++i) {
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < members.size()) {
      llvm::BasicBlock *block = this->lowering.functionFrame().newBlock("union.field");
      next = this->lowering.functionFrame().newBlock("union.field.next");
      this->lowering.builder.CreateCondBr(this->lowering.unionLowerer().emitIsOneOf(id, members[i]),
                                          block, next);
      this->lowering.functionFrame().switchTo(block);
    }
    incoming.push_back({this->lowering.objectLayouts().fieldAddress(object, members[i], name),
                        this->lowering.builder.GetInsertBlock()});
    this->lowering.builder.CreateBr(endBlock);
    if (next) {
      this->lowering.functionFrame().switchTo(next);
    }
  }
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.builder.getPtrTy(),
                                                        unsigned(incoming.size()));
  for (auto [v, block] : incoming) {
    phi->addIncoming(v, block);
  }

  return phi;
}

llvm::Value *UnionMemberLowerer::emitUnionFieldRead(MemberExpr *member) {
  const Type *type = member->object->type;
  Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
  llvm::ArrayRef<const Type *> members = type->members();
  const Type *fieldType = members.front()->field(member->name)->type;
  bool same = llvm::all_of(
      members, [&](const Type *m) { return m->field(member->name)->type == fieldType; });
  llvm::Value *result;
  if (same) {
    llvm::Value *address = this->unionFieldAddress(object.value, type, member->name);
    result = this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(fieldType),
                                               address, member->name);
    this->lowering.refCounts().retainValue(result, fieldType);
    result = this->lowering.exprLowerer().emitConversion(result, fieldType, member->type);
  } else {
    llvm::Value *id = this->lowering.unionLowerer().objectClassId(object.value);
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("union.field.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (size_t i = 0; i < members.size(); ++i) {
      const Type *m = members[i];
      llvm::BasicBlock *next = nullptr;
      if (i + 1 < members.size()) {
        llvm::BasicBlock *read = this->lowering.functionFrame().newBlock("union.field");
        next = this->lowering.functionFrame().newBlock("union.field.next");
        this->lowering.builder.CreateCondBr(this->lowering.unionLowerer().emitIsOneOf(id, m), read,
                                            next);
        this->lowering.functionFrame().switchTo(read);
      }
      const Type *own = m->field(member->name)->type;
      llvm::Value *value = this->lowering.builder.CreateLoad(
          this->lowering.moduleBuilder().lower(own),
          this->lowering.objectLayouts().fieldAddress(object.value, m, member->name), member->name);
      this->lowering.refCounts().retainValue(value, own);
      incoming.push_back({this->lowering.exprLowerer().emitConversion(value, own, member->type),
                          this->lowering.builder.GetInsertBlock()});
      this->lowering.builder.CreateBr(endBlock);
      if (next) {
        this->lowering.functionFrame().switchTo(next);
      }
    }
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(
        this->lowering.moduleBuilder().lower(member->type), unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  this->lowering.refCounts().dropOperand(object);

  return result;
}

llvm::Value *UnionMemberLowerer::emitObjectIn(BinaryExpr *bin) {
  this->lowering.stmtLowerer().emitDiscarded(bin->lhs);
  const Type *type = bin->rhs->type;
  Lowering::Operand value = this->lowering.refCounts().emitOperand(bin->rhs, true);
  std::string name;
  const Expr *key = bin->lhs;
  while (auto *conv = dyn_cast<CastExpr>(key)) {
    key = conv->operand;
  }
  auto *text = dyn_cast<StringLitExpr>(key);
  if (text) {
    name = ToUtf8::toUtf8(text->value);
  }
  llvm::Value *object = value.value;
  llvm::Value *result;
  const Type *objects = type->nonOptional();
  llvm::Value *present = type->isOptional()
                             ? this->lowering.optionalLowerer().optionalHas(object, type)
                             : this->lowering.builder.getTrue();
  if (type->isOptional()) {
    object = this->lowering.optionalLowerer().optionalValue(object, type);
  }
  std::vector<const Type *> with;
  if (objects->isTagged()) {
    llvm::Value *isObject = this->lowering.builder.CreateICmpEQ(
        this->lowering.unionLayout().unionTag(object),
        this->lowering.builder.getInt32(unsigned(UnionSlot::Object)));
    present = this->lowering.builder.CreateAnd(present, isObject);
    objects = objects->slotPart(UnionSlot::Object);
    object = this->lowering.builder.CreateSelect(
        isObject,
        this->lowering.unionLayout().taggedPart(object, value.type->nonOptional(),
                                                UnionSlot::Object),
        llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy()));
  }
  for (const Type *m :
       objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(objects)) {
    if (m->isObject() && m->field(name)) {
      with.push_back(m);
    }
  }
  llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *testBlock = this->lowering.functionFrame().newBlock("in.test");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("in.end");
  this->lowering.builder.CreateCondBr(present, testBlock, endBlock);
  this->lowering.functionFrame().switchTo(testBlock);
  llvm::Value *matches = this->lowering.unionLowerer().emitIsOneOf(
      this->lowering.unionLowerer().objectClassId(object), with);
  llvm::BasicBlock *testEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.builder.getInt1Ty(), 2);
  phi->addIncoming(this->lowering.builder.getFalse(), before);
  phi->addIncoming(matches, testEnd);
  result = phi;
  this->lowering.refCounts().dropOperand(value);

  return result;
}

llvm::Value *UnionMemberLowerer::emitUnionMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *fnType = member->type;
  bool borrow = ClassLowerer::staysDuringCall(member->object) &&
                llvm::all_of(call->args, AssignLowerer::cannotAssign);
  Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, borrow);
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i) {
    values.push_back(this->lowering.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *id = this->lowering.unionLowerer().objectClassId(self.value);
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("union.call.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (size_t i = 0; i < call->unionMethods().size(); ++i) {
    const UnionMethod &m = call->unionMethods()[i];
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < call->unionMethods().size()) {
      llvm::BasicBlock *block = this->lowering.functionFrame().newBlock("union.call");
      next = this->lowering.functionFrame().newBlock("union.call.next");
      this->lowering.builder.CreateCondBr(this->lowering.unionLowerer().emitIsOneOf(id, m.member),
                                          block, next);
      this->lowering.functionFrame().switchTo(block);
    }
    llvm::Value *result;
    if (m.method) {
      std::vector<llvm::Value *> args{self.value};
      args.insert(args.end(), values.begin(), values.end());
      result =
          m.virtualCall
              ? this->lowering.builder.CreateCall(this->lowering.closureLowerer().closureFnType(
                                                      ClosureLowerer::functionTypeOf(m.method)),
                                                  this->lowering.inheritLowerer().vtableEntry(
                                                      self.value, m.method->decl->slot(), false),
                                                  args)
              : this->lowering.builder.CreateCall(
                    this->lowering.classDescriptors().methodCode(m.method->decl), args);
    } else {
      llvm::Value *fn = this->lowering.builder.CreateLoad(
          this->lowering.fnTy,
          this->lowering.objectLayouts().fieldAddress(self.value, m.member, member->name));
      result = this->lowering.closureLowerer().callFunctionValue(fn, fnType, values);
    }
    if (result && !result->getType()->isVoidTy()) {
      incoming.push_back({result, this->lowering.builder.GetInsertBlock()});
    }
    this->lowering.builder.CreateBr(endBlock);
    if (next) {
      this->lowering.functionFrame().switchTo(next);
    }
  }
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::Value *result = nullptr;
  if (!incoming.empty()) {
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(incoming.front().first->getType(),
                                                          unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    result = phi;
  }
  this->lowering.refCounts().dropOperand(self);

  return result;
}

}
