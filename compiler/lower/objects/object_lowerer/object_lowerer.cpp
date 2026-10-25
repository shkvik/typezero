#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

ObjectLowerer::ObjectLowerer(CodeGen &codegen) : codegen_(codegen) {}

// `{ x: 1, y }`: the values in the order written, then undefined in the
// optional fields the literal leaves out.
llvm::Value *ObjectLowerer::emitObjectLiteral(ObjectLitExpr *lit) {
  if (lit->type->isJson()) {
    return codegen_.jsonLowerer().emitJsonObjectLiteral(lit);
  }
  if (lit->type->isRecord()) {
    return codegen_.dictLowerer().emitRecordLiteral(lit);
  }
  if (lit->cls) {
    return codegen_.classLowerer().emitMethodLiteral(lit);
  }
  if (lit->hasSpread()) {
    return emitSpreadLiteral(lit);
  }
  const Type *type = lit->type;
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  std::vector<unsigned> keys;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    keys.push_back(ObjectLayouts::fieldIndex(type, prop.name));
  }
  for (unsigned i = 0; i < type->fields().size(); ++i) {
    if (!llvm::is_contained(keys, i)) {
      keys.push_back(i);
    }
  }
  // If a value may throw, the values come first (held, see emitHeldValues),
  // then the object: one that a throw left half filled could not be
  // released.
  std::vector<Expr *> exprs;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    exprs.push_back(prop.value);
  }
  std::vector<llvm::Value *> values;
  if (llvm::any_of(exprs, [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); })) {
    values = codegen_.throwLowerer().emitHeldValues(exprs);
  }
  bool member = codegen_.makesMember(lit, type);
  llvm::Value *object = codegen_.objectLayouts().allocObject(type, keys, member);
  codegen_.groupLowerer().noteUnstored(object, lit);
  // Nothing can reach the new object while its values are evaluated.
  for (size_t k = 0; k < keys.size(); ++k) {
    unsigned i = keys[k];
    llvm::Value *value = k < values.size()       ? values[k]
                         : k < lit->props.size() ? codegen_.exprLowerer().emitExpr(lit->props[k].value)
                                                 : codegen_.optionalLowerer().undefinedValue(type->fields()[i].type);
    codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(layout.type, object, layout.elements[i]));
    if (member) {
      codegen_.storeLowerer().groupStore(object, value, type->fields()[i].type);
    }
  }

  return object;
}

// `{ ...a, x: 1, ...b }`. The object is made first with every field empty
// (zero, which releases as nothing), and each part stores its values in
// order, replacing what an earlier part stored. A spread reads the fields of
// its object when it comes, as in JavaScript, but does not copy an optional
// field that holds undefined. The keys are in the order the parts first give
// them, the fields of a spread object in the order of its type.
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

  llvm::Value *object = codegen_.objectLayouts().allocObject(type, keys, codegen_.makesMember(lit, type));
  codegen_.groupLowerer().noteUnstored(object, lit);
  for (const Field &f : fields) {
    codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(f.type)),
                         codegen_.objectLayouts().fieldAddress(object, type, f.name, /*exact=*/true));
  }
  // Nothing can reach the new object while the parts are evaluated; a part
  // that throws leaves it with what is stored so far, which the way up
  // releases (every field is a value or empty).
  CodeGen::Held held = codegen_.throwLowerer().hold(object, type, llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) {
                     return codegen_.mayThrow().exprThrows(p.value);
                   }));
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      if (prop.replaced) {
        codegen_.stmtLowerer().emitDiscarded(prop.value);
        continue;
      }
      const Type *to = type->field(prop.name)->type;
      codegen_.storeLowerer().storeIn(object, type, codegen_.objectLayouts().fieldAddress(object, type, prop.name, /*exact=*/true),
              codegen_.exprLowerer().emitExpr(prop.value), to);
      continue;
    }
    const Type *source = prop.value->type;
    // Its fields are copied right away, so a variable can be borrowed.
    CodeGen::Operand from = codegen_.refCounts().emitOperand(prop.value, /*canBorrow=*/true);
    for (const Field &f : source->fields()) {
      if (llvm::is_contained(prop.omit, f.name) ||
          llvm::is_contained(prop.replacedFields, f.name)) {
        continue;
      }
      const Type *to = type->field(f.name)->type;
      llvm::Value *address = codegen_.objectLayouts().fieldAddress(object, type, f.name, /*exact=*/true);
      // A method of an interface that some class implements: bound to the
      // object.
      if (const CodeGen::InterfaceMember *method = codegen_.inheritLowerer().dispatchedMember(source, f.name);
          method && method->callable) {
        llvm::Value *info = codegen_.builder_.CreateLoad(
            codegen_.builder_.getPtrTy(), codegen_.inheritLowerer().tableEntry(from.value, source, unsigned(method->entry + 1)));
        codegen_.refCounts().retainValue(from.value, source);
        codegen_.storeLowerer().storeIn(object, type, address, codegen_.closureLowerer().makeFunctionValue(info, from.value), to);
        continue;
      }
      llvm::Value *value =
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(f.type), codegen_.objectLayouts().fieldAddress(from.value, source, f.name), f.name);
      if (!f.type->isOptional()) {
        codegen_.refCounts().retainValue(value, f.type);
        codegen_.storeLowerer().storeIn(object, type, address, codegen_.exprLowerer().emitConversion(value, f.type, to), to);
        continue;
      }
      llvm::BasicBlock *copyBlock = codegen_.functionFrame().newBlock("spread.copy");
      llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("spread.next");
      codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value, f.type), copyBlock, nextBlock);
      codegen_.functionFrame().switchTo(copyBlock);
      const Type *inner = f.type->inner();
      llvm::Value *present = codegen_.optionalLowerer().optionalValue(value, f.type);
      codegen_.refCounts().retainValue(present, inner);
      codegen_.storeLowerer().storeIn(object, type, address, codegen_.exprLowerer().emitConversion(present, inner, to), to);
      codegen_.builder_.CreateBr(nextBlock);
      codegen_.functionFrame().switchTo(nextBlock);
    }
    codegen_.refCounts().dropOperand(from);
  }

  return codegen_.throwLowerer().take(held);
}

llvm::Value *ObjectLowerer::emitTupleLiteral(ArrayLitExpr *lit) {
  const Type *type = lit->type;
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  // As for an object literal: if an element may throw, the elements come
  // first (held), then the tuple, which a throw would leave half filled.
  std::vector<Expr *> exprs;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    exprs.push_back(e.expr);
  }
  std::vector<llvm::Value *> values;
  if (llvm::any_of(exprs, [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); })) {
    values = codegen_.throwLowerer().emitHeldValues(exprs);
  }
  bool member = codegen_.makesMember(lit, type);
  llvm::Value *object = codegen_.objectLayouts().allocObject(type, keys, member);
  codegen_.groupLowerer().noteUnstored(object, lit);
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *value = i < values.size() ? values[i] : codegen_.exprLowerer().emitExpr(exprs[i]);
    codegen_.builder_.CreateStore(value, codegen_.objectLayouts().fieldAddress(object, type, type->fields()[i].name));
    if (member) {
      codegen_.storeLowerer().groupStore(object, value, type->fields()[i].type);
    }
  }

  return object;
}

void ObjectLowerer::emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                              const Type *element) {
  for (const Field &f : tupleType->fields()) {
    llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(f.type), codegen_.objectLayouts().fieldAddress(tuple, tupleType, f.name));
    codegen_.refCounts().retainValue(value, f.type);
    codegen_.arrayLowerer().emitPush(array, {codegen_.exprLowerer().emitConversion(value, f.type, element)}, element);
  }
}

// Object.keys, Object.values and Object.entries: the runtime walks the
// fields in the order of the object's keys, leaving out the optional ones
// that hold undefined.
llvm::Value *ObjectLowerer::emitObjectCall(CallExpr *call) {
  if (call->args[0]->type->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonObjectCall(call);
  }
  if (call->args[0]->type->isDict()) {
    return codegen_.dictMethodLowerer().emitDictObjectCall(call);
  }
  CodeGen::Operand object = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = codegen_.builder_.CreateCall(codegen_.rt_.objKeys, {object.value, codegen_.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = codegen_.builder_.CreateCall(codegen_.rt_.objValues, {object.value, codegen_.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    // Each entry is a new tuple [key, value].
    uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.objectLayouts().objectLayout(element).type);
    result = codegen_.builder_.CreateCall(codegen_.rt_.objEntries,
                                 {object.value, codegen_.typeDescriptors().typeDescriptor(element),
                                  codegen_.objectLayouts().layoutDescriptor(element, {0, 1}), codegen_.builder_.getInt64(size)},
                                 "entries");
  }
  codegen_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *ObjectLowerer::emitFieldRead(MemberExpr *member) {
  if (member->object->type->isUnion()) { // a field each member has
    return codegen_.unionMemberLowerer().emitUnionFieldRead(member);
  }
  // A method of an interface, as a value: bound to the object, with the info
  // of the code the object's table gives.
  if (member->signature >= 0 &&
      !codegen_.inheritLowerer().dispatchedMember(member->object->type, member->name, member->signature)) {
    // Of an interface that no class implements (see emitInterfaceCall).
    codegen_.stmtLowerer().emitDiscarded(member->object);
    codegen_.builder_.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(codegen_.module_.get(), llvm::Intrinsic::trap));
    return llvm::PoisonValue::get(codegen_.fnTy_);
  }
  if (const CodeGen::InterfaceMember *method =
          codegen_.inheritLowerer().dispatchedMember(member->object->type, member->name, member->signature);
      method && method->callable) {
    llvm::Value *object = codegen_.exprLowerer().emitExpr(member->object);
    llvm::Value *info = codegen_.builder_.CreateLoad(
        codegen_.builder_.getPtrTy(), codegen_.inheritLowerer().tableEntry(object, member->object->type, unsigned(method->entry + 1)),
        member->name + ".info");
    return codegen_.closureLowerer().makeFunctionValue(info, object);
  }
  CodeGen::Operand object = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *value = codegen_.builder_.CreateLoad(
      codegen_.moduleBuilder().lower(member->type), codegen_.objectLayouts().fieldAddress(object.value, object.type, member->name), member->name);
  codegen_.refCounts().retainValue(value, member->type);
  codegen_.refCounts().dropOperand(object);

  return value;
}

// `o.x = v` and `o.x op= v`. As in JavaScript, the object is evaluated
// first, and the old value of a compound assignment is read before v.
llvm::Value *ObjectLowerer::keyedFieldAddress(llvm::Value *object, const Type *type, llvm::Value *key,
                                        const Type *keyType) {
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
    llvm::Value *field = codegen_.objectLayouts().fieldAddress(object, type, name);
    address = address
                  ? codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpEQ(key, codegen_.builder_.getInt32(n->atom())),
                                          field, address)
                  : field;
  }

  return address;
}

llvm::Value *ObjectLowerer::emitFieldAssign(AssignExpr *assign, bool needResult) {
  // `o[k] = v` with a key of literals, or `o.name = v`.
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  const Type *type = index ? index->type : member->type;
  Expr *objectExpr = index ? index->object : member->object;
  bool calm = AssignLowerer::cannotAssign(assign->value) && (!index || AssignLowerer::cannotAssign(index->index));
  CodeGen::Operand object = codegen_.refCounts().emitOperand(objectExpr, calm);
  llvm::Value *address = index ? keyedFieldAddress(object.value, object.type,
                                                   codegen_.exprLowerer().emitExpr(index->index), index->index->type)
                               : codegen_.objectLayouts().fieldAddress(object.value, object.type, member->name);
  llvm::Value *value;
  if (!assign->op) {
    value = codegen_.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.strTy_, address);
    codegen_.refCounts().retainString(old); // the right side may replace the field
    CodeGen::Operand rhs = codegen_.stringLowerer().emitConcatPart(assign->value, /*canBorrow=*/false);
    value = codegen_.stringLowerer().concatOperands({{old, true, type}, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), address);
    codegen_.refCounts().retainValue(old, type); // the right side may replace the field
    value = codegen_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    codegen_.refCounts().releaseValue(old, type);
  } else {
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), address);
    value = codegen_.binaryLowerer().emitArithmetic(*assign->op, old, codegen_.exprLowerer().emitExpr(assign->value), type);
  }
  // Debug mode notes the field a store into a member writes.
  codegen_.storeField_ = index ? index->field.str() : member->name.str();
  codegen_.storeLoc_ = assign->loc();
  codegen_.storeLowerer().storeIn(object.value, object.type, address, value, type);
  codegen_.storeField_.clear();
  codegen_.storeLoc_ = llvm::SMLoc();
  codegen_.refCounts().dropOperand(object);
  if (!needResult) {
    return nullptr;
  }
  codegen_.refCounts().retainValue(value, type);

  return value;
}

} // namespace tz
