#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::dyn_cast;

namespace tz {

ObjectLowerer::ObjectLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ObjectLowerer::emitObjectLiteral(ObjectLitExpr *lit) {
  if (lit->type->isJson()) {
    return this->lowering.jsonLowerer().emitJsonObjectLiteral(lit);
  }
  if (lit->type->isRecord()) {
    return this->lowering.dictLowerer().emitRecordLiteral(lit);
  }
  if (lit->cls) {
    return this->lowering.classLowerer().emitMethodLiteral(lit);
  }
  if (lit->hasSpread()) {
    return this->emitSpreadLiteral(lit);
  }
  const Type *type = lit->type;
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  std::vector<const ObjectLitExpr::Property *> named;
  std::vector<unsigned> keys;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      named.push_back(&prop);
      keys.push_back(ObjectLayouts::fieldIndex(type, prop.name));
    }
  }
  for (unsigned i = 0; i < type->fields().size(); ++i) {
    if (!llvm::is_contained(keys, i)) {
      keys.push_back(i);
    }
  }
  bool member = this->lowering.makesMember(lit, type);
  llvm::Value *object = this->lowering.objectLayouts().allocObject(type, keys, member);
  this->lowering.groupLowerer().noteUnstored(object, lit);
  std::vector<llvm::Value *> values;
  for (size_t k = 0; k < keys.size(); ++k) {
    unsigned i = keys[k];
    llvm::Value *value =
        k < named.size() ? this->lowering.exprLowerer().emitExpr(named[k]->value)
                         : this->lowering.optionalLowerer().undefinedValue(type->fields()[i].type);
    this->lowering.builder.CreateStore(
        value, this->lowering.builder.CreateStructGEP(layout.type, object, layout.elements[i]));
    values.push_back(value);
  }
  if (member) {
    for (size_t k = 0; k < keys.size(); ++k) {
      this->lowering.storeLowerer().groupStore(object, values[k], type->fields()[keys[k]].type);
    }
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      continue;
    }
    llvm::Value *value = this->lowering.exprLowerer().emitExpr(prop.value);
    llvm::Value *key = this->lowering.exprLowerer().emitExpr(prop.key());
    llvm::Value *address = this->keyedFieldAddress(object, type, key, prop.key()->type);
    this->lowering.storeLowerer().storeIn(object, type, address, value, prop.value->type);
  }

  return object;
}

llvm::Value *ObjectLowerer::emitSpreadLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  llvm::ArrayRef<Field> fields = type->fields();
  std::vector<unsigned> keys;
  auto addKey = [&](llvm::StringRef name) {
    unsigned i = ObjectLayouts::fieldIndex(type, name);
    if (!llvm::is_contained(keys, i)) {
      keys.push_back(i);
    }
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread()) {
      addKey(prop.name);
      continue;
    }
    for (const Field &f : prop.value->type->fields()) {
      if (!llvm::is_contained(prop.omit(), f.name)) {
        addKey(f.name);
      }
    }
  }
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (!llvm::is_contained(keys, i)) {
      keys.push_back(i);
    }
  }

  llvm::Value *object =
      this->lowering.objectLayouts().allocObject(type, keys, this->lowering.makesMember(lit, type));
  this->lowering.groupLowerer().noteUnstored(object, lit);
  for (const Field &f : fields) {
    this->lowering.builder.CreateStore(
        llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(f.type)),
        this->lowering.objectLayouts().fieldAddress(object, type, f.name, true));
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread()) {
      if (prop.replaced) {
        this->lowering.stmtLowerer().emitDiscarded(prop.value);
        continue;
      }
      const Type *to = type->field(prop.name)->type;
      this->lowering.storeLowerer().storeIn(
          object, type, this->lowering.objectLayouts().fieldAddress(object, type, prop.name, true),
          this->lowering.exprLowerer().emitExpr(prop.value), to);
      continue;
    }
    const Type *source = prop.value->type;
    Lowering::Operand from = this->lowering.refCounts().emitOperand(prop.value, true);
    for (const Field &f : source->fields()) {
      if (llvm::is_contained(prop.omit(), f.name) ||
          llvm::is_contained(prop.replacedFields(), f.name)) {
        continue;
      }
      const Type *to = type->field(f.name)->type;
      llvm::Value *address =
          this->lowering.objectLayouts().fieldAddress(object, type, f.name, true);
      if (const Lowering::InterfaceMember *method =
              this->lowering.inheritLowerer().dispatchedMember(source, f.name);
          method && method->callable) {
        llvm::Value *info =
            this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                              this->lowering.inheritLowerer().tableEntry(
                                                  from.value, source, unsigned(method->entry + 1)));
        this->lowering.refCounts().retainValue(from.value, source);
        this->lowering.storeLowerer().storeIn(
            object, type, address,
            this->lowering.closureLowerer().makeFunctionValue(info, from.value), to);
        continue;
      }
      llvm::Value *value = this->lowering.builder.CreateLoad(
          this->lowering.moduleBuilder().lower(f.type),
          this->lowering.objectLayouts().fieldAddress(from.value, source, f.name), f.name);
      if (!f.type->isOptional()) {
        this->lowering.refCounts().retainValue(value, f.type);
        this->lowering.storeLowerer().storeIn(
            object, type, address, this->lowering.exprLowerer().emitConversion(value, f.type, to),
            to);
        continue;
      }
      llvm::BasicBlock *copyBlock = this->lowering.functionFrame().newBlock("spread.copy");
      llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("spread.next");
      this->lowering.builder.CreateCondBr(
          this->lowering.optionalLowerer().optionalHas(value, f.type), copyBlock, nextBlock);
      this->lowering.functionFrame().switchTo(copyBlock);
      const Type *inner = f.type->inner();
      llvm::Value *present = this->lowering.optionalLowerer().optionalValue(value, f.type);
      this->lowering.refCounts().retainValue(present, inner);
      this->lowering.storeLowerer().storeIn(
          object, type, address, this->lowering.exprLowerer().emitConversion(present, inner, to),
          to);
      this->lowering.builder.CreateBr(nextBlock);
      this->lowering.functionFrame().switchTo(nextBlock);
    }
    this->lowering.refCounts().dropOperand(from);
  }

  return object;
}

llvm::Value *ObjectLowerer::emitTupleLiteral(ArrayLitExpr *lit) {
  const Type *type = lit->type;
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  bool member = this->lowering.makesMember(lit, type);
  llvm::Value *object = this->lowering.objectLayouts().allocObject(type, keys, member);
  this->lowering.groupLowerer().noteUnstored(object, lit);
  std::vector<llvm::Value *> values;
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    llvm::Value *value = this->lowering.exprLowerer().emitExpr(lit->elements[i].expr);
    this->lowering.builder.CreateStore(
        value, this->lowering.objectLayouts().fieldAddress(object, type, type->fields()[i].name));
    values.push_back(value);
  }
  if (member) {
    for (size_t i = 0; i < values.size(); ++i) {
      this->lowering.storeLowerer().groupStore(object, values[i], type->fields()[i].type);
    }
  }

  return object;
}

void ObjectLowerer::emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                                    const Type *element) {
  for (const Field &f : tupleType->fields()) {
    llvm::Value *value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(f.type),
        this->lowering.objectLayouts().fieldAddress(tuple, tupleType, f.name));
    this->lowering.refCounts().retainValue(value, f.type);
    this->lowering.arrayLowerer().emitPush(
        array, {this->lowering.exprLowerer().emitConversion(value, f.type, element)}, element);
  }
}

llvm::Value *ObjectLowerer::emitObjectCall(CallExpr *call) {
  if (call->args[0]->type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonObjectCall(call);
  }
  if (call->args[0]->type->isDict()) {
    return this->lowering.dictMethodLowerer().emitDictObjectCall(call);
  }
  Lowering::Operand object = this->lowering.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin() == Builtin::ObjectKeys) {
    result = this->lowering.builder.CreateCall(
        this->lowering.rt_.objKeys,
        {object.value, this->lowering.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin() == Builtin::ObjectValues) {
    result = this->lowering.builder.CreateCall(
        this->lowering.rt_.objValues,
        {object.value, this->lowering.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
        this->lowering.objectLayouts().objectLayout(element).type);
    result = this->lowering.builder.CreateCall(
        this->lowering.rt_.objEntries,
        {object.value, this->lowering.typeDescriptors().typeDescriptor(element),
         this->lowering.objectLayouts().layoutDescriptor(element, {0, 1}),
         this->lowering.builder.getInt64(size)},
        "entries");
  }
  this->lowering.refCounts().dropOperand(object);

  return result;
}

llvm::Value *ObjectLowerer::emitFieldRead(MemberExpr *member) {
  if (member->object->type->isUnion()) {
    return this->lowering.unionMemberLowerer().emitUnionFieldRead(member);
  }
  if (member->signature() >= 0 && !this->lowering.inheritLowerer().dispatchedMember(
                                      member->object->type, member->name, member->signature())) {
    this->lowering.stmtLowerer().emitDiscarded(member->object);
    this->lowering.builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
        this->lowering.module_.get(), llvm::Intrinsic::trap));
    return llvm::PoisonValue::get(this->lowering.fnTy);
  }
  if (const Lowering::InterfaceMember *method = this->lowering.inheritLowerer().dispatchedMember(
          member->object->type, member->name, member->signature());
      method && method->callable) {
    llvm::Value *object = this->lowering.exprLowerer().emitExpr(member->object);
    llvm::Value *info = this->lowering.builder.CreateLoad(
        this->lowering.builder.getPtrTy(),
        this->lowering.inheritLowerer().tableEntry(object, member->object->type,
                                                   unsigned(method->entry + 1)),
        member->name + ".info");
    return this->lowering.closureLowerer().makeFunctionValue(info, object);
  }
  Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
  llvm::Value *value = this->lowering.builder.CreateLoad(
      this->lowering.moduleBuilder().lower(member->type),
      this->lowering.objectLayouts().fieldAddress(object.value, object.type, member->name),
      member->name);
  this->lowering.refCounts().retainValue(value, member->type);
  this->lowering.refCounts().dropOperand(object);

  return value;
}

llvm::Value *ObjectLowerer::keyedFieldAddress(llvm::Value *object, const Type *type,
                                              llvm::Value *key, const Type *keyType) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  llvm::Value *address = nullptr;
  for (const Type *n : llvm::reverse(names)) {
    std::string name;
    const std::u16string &text = n->literalText();
    name = ToUtf8::toUtf8(text);
    llvm::Value *field = this->lowering.objectLayouts().fieldAddress(object, type, name);
    address = address ? this->lowering.builder.CreateSelect(
                            this->lowering.builder.CreateICmpEQ(
                                key, this->lowering.builder.getInt32(n->atom())),
                            field, address)
                      : field;
  }

  return address;
}

llvm::Value *ObjectLowerer::emitFieldAssign(AssignExpr *assign, bool needResult) {
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  const Type *type = index ? index->type : member->type;
  Expr *objectExpr = index ? index->object : member->object;
  bool calm = AssignLowerer::cannotAssign(assign->value) &&
              (!index || AssignLowerer::cannotAssign(index->index));
  Lowering::Operand object = this->lowering.refCounts().emitOperand(objectExpr, calm);
  llvm::Value *address =
      index ? this->keyedFieldAddress(object.value, object.type,
                                      this->lowering.exprLowerer().emitExpr(index->index),
                                      index->index->type)
            : this->lowering.objectLayouts().fieldAddress(object.value, object.type, member->name);
  llvm::Value *value;
  if (!assign->op) {
    value = this->lowering.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    llvm::Value *old = this->lowering.builder.CreateLoad(this->lowering.strTy, address);
    this->lowering.refCounts().retainString(old);
    Lowering::Operand rhs = this->lowering.stringLowerer().emitConcatPart(assign->value, false);
    value = this->lowering.stringLowerer().concatOperands({{old, true, type}, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old =
        this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(type), address);
    this->lowering.refCounts().retainValue(old, type);
    value = this->lowering.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    this->lowering.refCounts().releaseValue(old, type);
  } else {
    llvm::Value *old =
        this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(type), address);
    value = this->lowering.binaryLowerer().emitArithmetic(
        *assign->op, old, this->lowering.exprLowerer().emitExpr(assign->value), type,
        assign->rangeProven);
  }
  this->lowering.storeField = index ? index->field().str() : member->name.str();
  this->lowering.storeLoc = assign->loc();
  this->lowering.storeLowerer().storeIn(object.value, object.type, address, value, type);
  this->lowering.storeField.clear();
  this->lowering.storeLoc = llvm::SMLoc();
  this->lowering.refCounts().dropOperand(object);
  if (!needResult) {
    return nullptr;
  }
  this->lowering.refCounts().retainValue(value, type);

  return value;
}

}
