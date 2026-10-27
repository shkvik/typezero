#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
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
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ObjectLowerer::ObjectLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ObjectLowerer::emitObjectLiteral(ObjectLitExpr *lit) {
  if (lit->type->isJson()) {
    return lowering_.jsonLowerer().emitJsonObjectLiteral(lit);
  }
  if (lit->type->isRecord()) {
    return lowering_.dictLowerer().emitRecordLiteral(lit);
  }
  if (lit->cls) {
    return lowering_.classLowerer().emitMethodLiteral(lit);
  }
  if (lit->hasSpread()) {
    return emitSpreadLiteral(lit);
  }
  const Type *type = lit->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  std::vector<unsigned> keys;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    keys.push_back(ObjectLayouts::fieldIndex(type, prop.name));
  }
  for (unsigned i = 0; i < type->fields().size(); ++i) {
    if (!llvm::is_contained(keys, i)) {
      keys.push_back(i);
    }
  }
  bool member = lowering_.makesMember(lit, type);
  llvm::Value *object = lowering_.objectLayouts().allocObject(type, keys, member);
  lowering_.groupLowerer().noteUnstored(object, lit);
  for (size_t k = 0; k < keys.size(); ++k) {
    unsigned i = keys[k];
    llvm::Value *value = k < lit->props.size()
                             ? lowering_.exprLowerer().emitExpr(lit->props[k].value)
                             : lowering_.optionalLowerer().undefinedValue(type->fields()[i].type);
    lowering_.builder_.CreateStore(
        value, lowering_.builder_.CreateStructGEP(layout.type, object, layout.elements[i]));
    if (member) {
      lowering_.storeLowerer().groupStore(object, value, type->fields()[i].type);
    }
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
    if (!prop.spread) {
      addKey(prop.name);
      continue;
    }
    for (const Field &f : prop.value->type->fields()) {
      if (!llvm::is_contained(prop.omit, f.name)) {
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
      lowering_.objectLayouts().allocObject(type, keys, lowering_.makesMember(lit, type));
  lowering_.groupLowerer().noteUnstored(object, lit);
  for (const Field &f : fields) {
    lowering_.builder_.CreateStore(
        llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(f.type)),
        lowering_.objectLayouts().fieldAddress(object, type, f.name, true));
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      if (prop.replaced) {
        lowering_.stmtLowerer().emitDiscarded(prop.value);
        continue;
      }
      const Type *to = type->field(prop.name)->type;
      lowering_.storeLowerer().storeIn(
          object, type, lowering_.objectLayouts().fieldAddress(object, type, prop.name, true),
          lowering_.exprLowerer().emitExpr(prop.value), to);
      continue;
    }
    const Type *source = prop.value->type;
    Lowering::Operand from = lowering_.refCounts().emitOperand(prop.value, true);
    for (const Field &f : source->fields()) {
      if (llvm::is_contained(prop.omit, f.name) ||
          llvm::is_contained(prop.replacedFields, f.name)) {
        continue;
      }
      const Type *to = type->field(f.name)->type;
      llvm::Value *address = lowering_.objectLayouts().fieldAddress(object, type, f.name, true);
      if (const Lowering::InterfaceMember *method =
              lowering_.inheritLowerer().dispatchedMember(source, f.name);
          method && method->callable) {
        llvm::Value *info = lowering_.builder_.CreateLoad(
            lowering_.builder_.getPtrTy(),
            lowering_.inheritLowerer().tableEntry(from.value, source, unsigned(method->entry + 1)));
        lowering_.refCounts().retainValue(from.value, source);
        lowering_.storeLowerer().storeIn(
            object, type, address, lowering_.closureLowerer().makeFunctionValue(info, from.value),
            to);
        continue;
      }
      llvm::Value *value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(f.type),
          lowering_.objectLayouts().fieldAddress(from.value, source, f.name), f.name);
      if (!f.type->isOptional()) {
        lowering_.refCounts().retainValue(value, f.type);
        lowering_.storeLowerer().storeIn(
            object, type, address, lowering_.exprLowerer().emitConversion(value, f.type, to), to);
        continue;
      }
      llvm::BasicBlock *copyBlock = lowering_.functionFrame().newBlock("spread.copy");
      llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("spread.next");
      lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value, f.type),
                                      copyBlock, nextBlock);
      lowering_.functionFrame().switchTo(copyBlock);
      const Type *inner = f.type->inner();
      llvm::Value *present = lowering_.optionalLowerer().optionalValue(value, f.type);
      lowering_.refCounts().retainValue(present, inner);
      lowering_.storeLowerer().storeIn(
          object, type, address, lowering_.exprLowerer().emitConversion(present, inner, to), to);
      lowering_.builder_.CreateBr(nextBlock);
      lowering_.functionFrame().switchTo(nextBlock);
    }
    lowering_.refCounts().dropOperand(from);
  }

  return object;
}

llvm::Value *ObjectLowerer::emitTupleLiteral(ArrayLitExpr *lit) {
  const Type *type = lit->type;
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  bool member = lowering_.makesMember(lit, type);
  llvm::Value *object = lowering_.objectLayouts().allocObject(type, keys, member);
  lowering_.groupLowerer().noteUnstored(object, lit);
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    llvm::Value *value = lowering_.exprLowerer().emitExpr(lit->elements[i].expr);
    lowering_.builder_.CreateStore(
        value, lowering_.objectLayouts().fieldAddress(object, type, type->fields()[i].name));
    if (member) {
      lowering_.storeLowerer().groupStore(object, value, type->fields()[i].type);
    }
  }

  return object;
}

void ObjectLowerer::emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                                    const Type *element) {
  for (const Field &f : tupleType->fields()) {
    llvm::Value *value = lowering_.builder_.CreateLoad(
        lowering_.moduleBuilder().lower(f.type),
        lowering_.objectLayouts().fieldAddress(tuple, tupleType, f.name));
    lowering_.refCounts().retainValue(value, f.type);
    lowering_.arrayLowerer().emitPush(
        array, {lowering_.exprLowerer().emitConversion(value, f.type, element)}, element);
  }
}

llvm::Value *ObjectLowerer::emitObjectCall(CallExpr *call) {
  if (call->args[0]->type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonObjectCall(call);
  }
  if (call->args[0]->type->isDict()) {
    return lowering_.dictMethodLowerer().emitDictObjectCall(call);
  }
  Lowering::Operand object = lowering_.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = lowering_.builder_.CreateCall(
        lowering_.rt_.objKeys, {object.value, lowering_.typeDescriptors().typeDescriptor(element)},
        "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = lowering_.builder_.CreateCall(
        lowering_.rt_.objValues,
        {object.value, lowering_.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(
        lowering_.objectLayouts().objectLayout(element).type);
    result = lowering_.builder_.CreateCall(
        lowering_.rt_.objEntries,
        {object.value, lowering_.typeDescriptors().typeDescriptor(element),
         lowering_.objectLayouts().layoutDescriptor(element, {0, 1}),
         lowering_.builder_.getInt64(size)},
        "entries");
  }
  lowering_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *ObjectLowerer::emitFieldRead(MemberExpr *member) {
  if (member->object->type->isUnion()) {
    return lowering_.unionMemberLowerer().emitUnionFieldRead(member);
  }
  if (member->signature >= 0 && !lowering_.inheritLowerer().dispatchedMember(
                                    member->object->type, member->name, member->signature)) {
    lowering_.stmtLowerer().emitDiscarded(member->object);
    lowering_.builder_.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(lowering_.module_.get(), llvm::Intrinsic::trap));
    return llvm::PoisonValue::get(lowering_.fnTy_);
  }
  if (const Lowering::InterfaceMember *method = lowering_.inheritLowerer().dispatchedMember(
          member->object->type, member->name, member->signature);
      method && method->callable) {
    llvm::Value *object = lowering_.exprLowerer().emitExpr(member->object);
    llvm::Value *info = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(),
        lowering_.inheritLowerer().tableEntry(object, member->object->type,
                                              unsigned(method->entry + 1)),
        member->name + ".info");
    return lowering_.closureLowerer().makeFunctionValue(info, object);
  }
  Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, true);
  llvm::Value *value = lowering_.builder_.CreateLoad(
      lowering_.moduleBuilder().lower(member->type),
      lowering_.objectLayouts().fieldAddress(object.value, object.type, member->name),
      member->name);
  lowering_.refCounts().retainValue(value, member->type);
  lowering_.refCounts().dropOperand(object);

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
    llvm::convertUTF16ToUTF8String(
        llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()),
                                    text.size()),
        name);
    llvm::Value *field = lowering_.objectLayouts().fieldAddress(object, type, name);
    address =
        address ? lowering_.builder_.CreateSelect(
                      lowering_.builder_.CreateICmpEQ(key, lowering_.builder_.getInt32(n->atom())),
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
  Lowering::Operand object = lowering_.refCounts().emitOperand(objectExpr, calm);
  llvm::Value *address =
      index ? keyedFieldAddress(object.value, object.type,
                                lowering_.exprLowerer().emitExpr(index->index), index->index->type)
            : lowering_.objectLayouts().fieldAddress(object.value, object.type, member->name);
  llvm::Value *value;
  if (!assign->op) {
    value = lowering_.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    llvm::Value *old = lowering_.builder_.CreateLoad(lowering_.strTy_, address);
    lowering_.refCounts().retainString(old);
    Lowering::Operand rhs = lowering_.stringLowerer().emitConcatPart(assign->value, false);
    value = lowering_.stringLowerer().concatOperands({{old, true, type}, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), address);
    lowering_.refCounts().retainValue(old, type);
    value = lowering_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    lowering_.refCounts().releaseValue(old, type);
  } else {
    llvm::Value *old =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), address);
    value = lowering_.binaryLowerer().emitArithmetic(
        *assign->op, old, lowering_.exprLowerer().emitExpr(assign->value), type);
  }
  lowering_.storeField_ = index ? index->field.str() : member->name.str();
  lowering_.storeLoc_ = assign->loc();
  lowering_.storeLowerer().storeIn(object.value, object.type, address, value, type);
  lowering_.storeField_.clear();
  lowering_.storeLoc_ = llvm::SMLoc();
  lowering_.refCounts().dropOperand(object);
  if (!needResult) {
    return nullptr;
  }
  lowering_.refCounts().retainValue(value, type);

  return value;
}

}
