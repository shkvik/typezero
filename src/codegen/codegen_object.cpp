// Objects: their layouts, the descriptors the runtime gets, literals, and
// reading and writing fields.
//
// An object is a reference to { i64 rc, ptr layout, fields... }, allocated by
// tz_obj_alloc (runtime/closure.c) as the environments of closures are. The
// fields are stored unboxed, each in the representation of its type (an
// optional field as `T | undefined`), at offsets fixed by the type, so
// reading one is a single load.
//
// `layout` points at a constant descriptor (TzLayout in runtime/tzrt.h): the
// function that drops the object, and for printing the names, offsets and
// types of the fields in the order of the literal that created the object.
// Node prints the keys of an object in the order they were added, so one type
// may have several descriptors, which differ only in that order.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"

#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The position of the field `name` in Type::fields().
unsigned fieldIndex(const Type *type, llvm::StringRef name) {
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i)
    if (fields[i].name == name)
      return i;
  llvm_unreachable("Sema checked that the field exists");
}

// A name for the functions of a layout in the IR.
std::string layoutName(const Type *type) {
  llvm::StringRef name = type->name();
  return name.starts_with("{") ? "object" : name.str();
}

} // namespace

const CodeGen::ObjectLayout &CodeGen::objectLayout(const Type *type) {
  auto [it, inserted] = objectLayouts_.try_emplace(type);
  ObjectLayout &layout = it->second;
  if (!inserted)
    return layout;

  // By alignment, largest first, so that each field starts where the one
  // before it ends; then by name, so that the order does not depend on the
  // declaration.
  llvm::ArrayRef<Field> fields = type->fields();
  const llvm::DataLayout &data = module_->getDataLayout();
  std::vector<unsigned> order(fields.size());
  std::iota(order.begin(), order.end(), 0u);
  llvm::stable_sort(order, [&](unsigned a, unsigned b) {
    uint64_t x = data.getABITypeAlign(lower(fields[a].type)).value();
    uint64_t y = data.getABITypeAlign(lower(fields[b].type)).value();
    return x != y ? x > y : fields[a].name < fields[b].name;
  });
  std::vector<llvm::Type *> elements{builder_.getInt64Ty(), builder_.getPtrTy()};
  layout.elements.resize(fields.size());
  for (unsigned i : order) {
    layout.elements[i] = unsigned(elements.size());
    elements.push_back(lower(fields[i].type));
  }
  layout.type = llvm::StructType::get(ctx_, elements);

  auto *fnType = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  std::string name = layoutName(type);
  layout.drop = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage, "tz." + name + ".drop",
                                       module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);

  // The release: the count is the first word; the last reference drops the
  // object. Undefined (null) holds nothing.
  layout.release = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                          "tz." + name + ".release", module_.get());
  layout.release->addFnAttr(llvm::Attribute::AlwaysInline);
  layout.release->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", layout.release));
  auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", layout.release);
  auto *dropBlock = llvm::BasicBlock::Create(ctx_, "drop", layout.release);
  auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", layout.release);
  llvm::Value *object = layout.release->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *left = b.CreateSub(b.CreateLoad(b.getInt64Ty(), object, "rc"), b.getInt64(1));
  b.CreateStore(left, object);
  b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
  b.SetInsertPoint(dropBlock);
  b.CreateCall(layout.drop, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  emitObjectDrop(type, layout);
  return layout;
}

// Releases what the fields hold, then frees the object. A list (`next?:
// Node`) would make that recursion as deep as the list is long, so a field
// that holds an object of the same type is released last, by going round
// again: freeing a long list takes no stack.
void CodeGen::emitObjectDrop(const Type *type, ObjectLayout &layout) {
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *drop = layout.drop;
  auto *entry = llvm::BasicBlock::Create(ctx_, "entry", drop);
  auto *loopBlock = llvm::BasicBlock::Create(ctx_, "object", drop);
  builder_.SetInsertPoint(entry);
  builder_.CreateBr(loopBlock);
  builder_.SetInsertPoint(loopBlock);
  llvm::PHINode *object = builder_.CreatePHI(builder_.getPtrTy(), 2, "object");
  object->addIncoming(drop->getArg(0), entry);

  llvm::ArrayRef<Field> fields = type->fields();
  int next = -1; // the field of the same type, released last
  for (unsigned i = 0; i < fields.size(); ++i)
    if (fields[i].type->nonOptional() == type)
      next = int(i);
  std::vector<std::pair<llvm::Value *, const Type *>> values;
  llvm::Value *nextValue = nullptr;
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged())
      continue;
    llvm::Value *address = builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = builder_.CreateLoad(lower(fieldType), address, fields[i].name);
    if (int(i) == next)
      nextValue = value;
    else
      values.push_back({value, fieldType});
  }
  builder_.CreateCall(rt_.objFree, {object});
  for (auto [value, fieldType] : values)
    releaseValue(value, fieldType);
  if (nextValue) {
    auto *countedBlock = llvm::BasicBlock::Create(ctx_, "next", drop);
    auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", drop);
    builder_.CreateCondBr(builder_.CreateIsNotNull(nextValue), countedBlock, doneBlock);
    builder_.SetInsertPoint(countedBlock);
    llvm::Value *left = builder_.CreateSub(builder_.CreateLoad(builder_.getInt64Ty(), nextValue, "rc"),
                                           builder_.getInt64(1));
    builder_.CreateStore(left, nextValue);
    object->addIncoming(nextValue, countedBlock);
    builder_.CreateCondBr(builder_.CreateICmpEQ(left, builder_.getInt64(0)), loopBlock, doneBlock);
    builder_.SetInsertPoint(doneBlock);
  }
  builder_.CreateRetVoid();
  builder_.restoreIP(point);
}

llvm::Constant *CodeGen::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys) {
  llvm::Constant *&descriptor = layoutDescriptors_[{type, std::vector<unsigned>(keys.begin(), keys.end())}];
  if (descriptor)
    return descriptor;
  const ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> entries;
  for (unsigned k : keys) {
    const Field &field = type->fields()[k];
    uint64_t offset = offsets->getElementOffset(layout.elements[k]);
    entries.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(field.name), builder_.getInt32(uint32_t(field.name.size())),
                       builder_.getInt32(uint32_t(offset)), typeDescriptor(field.type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(fieldDescTy_, entries.size());
    auto *global = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arrayType, entries),
                                            "tz.fields." + layoutName(type));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  auto *global = new llvm::GlobalVariable(
      *module_, layoutDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(layoutDescTy_, {layout.drop, fields, builder_.getInt32(uint32_t(entries.size()))}),
      "tz.layout." + layoutName(type));
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;
  return descriptor;
}

llvm::Value *CodeGen::fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name) {
  const ObjectLayout &layout = objectLayout(type);
  return builder_.CreateStructGEP(layout.type, object, layout.elements[fieldIndex(type, name)], name + ".addr");
}

// `{ x: 1, y }`: the values in the order written, then undefined in the
// optional fields the literal leaves out.
llvm::Value *CodeGen::emitObjectLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  const ObjectLayout &layout = objectLayout(type);
  std::vector<unsigned> keys;
  for (const ObjectLitExpr::Property &prop : lit->props)
    keys.push_back(fieldIndex(type, prop.name));
  for (unsigned i = 0; i < type->fields().size(); ++i)
    if (!llvm::is_contained(keys, i))
      keys.push_back(i);
  llvm::Constant *descriptor = layoutDescriptor(type, keys);

  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "object");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  builder_.CreateStore(descriptor, builder_.CreateStructGEP(layout.type, object, 1));
  // Nothing can reach the new object while its values are evaluated.
  for (size_t k = 0; k < keys.size(); ++k) {
    unsigned i = keys[k];
    llvm::Value *value = k < lit->props.size() ? emitExpr(lit->props[k].value)
                                               : undefinedValue(type->fields()[i].type);
    builder_.CreateStore(value, builder_.CreateStructGEP(layout.type, object, layout.elements[i]));
  }
  return object;
}

llvm::Value *CodeGen::emitFieldRead(MemberExpr *member) {
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *value = builder_.CreateLoad(lower(member->type),
                                           fieldAddress(object.value, object.type, member->name), member->name);
  retainValue(value, member->type);
  dropOperand(object);
  return value;
}

// `o.x = v` and `o.x op= v`. As in JavaScript, the object is evaluated
// first, and the old value of a compound assignment is read before v.
llvm::Value *CodeGen::emitFieldAssign(AssignExpr *assign, bool needResult) {
  auto *member = cast<MemberExpr>(assign->target);
  const Type *type = member->type;
  Operand object = emitOperand(member->object, cannotAssign(assign->value));
  llvm::Value *address = fieldAddress(object.value, object.type, member->name);
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else if (type->isString()) {
    llvm::Value *old = builder_.CreateLoad(strTy_, address);
    retainString(old); // the right side may replace the field
    Operand rhs = emitConcatPart(assign->value, /*canBorrow=*/false);
    value = concatOperands({{old, true, type}, rhs});
  } else {
    llvm::Value *old = builder_.CreateLoad(lower(type), address);
    value = emitArithmetic(*assign->op, old, emitExpr(assign->value), type);
  }
  storeReplacing(address, value, type);
  dropOperand(object);
  if (!needResult)
    return nullptr;
  retainValue(value, type);
  return value;
}

} // namespace tz
