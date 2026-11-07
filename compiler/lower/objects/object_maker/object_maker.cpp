#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

const std::vector<llvm::StringRef> &ObjectMaker::orderOf(const Type *type,
                                                         llvm::ArrayRef<MakerFields> orders) {
  for (const MakerFields &entry : orders) {
    const ClassDecl *cls = type->classDecl();
    if (cls) {
      if (entry.cls && cls->name == entry.cls) {
        return entry.fields;
      }
      continue;
    }
    if (entry.cls) {
      continue;
    }
    auto listed = [&](const Field &f) {
      return llvm::is_contained(entry.fields, f.name) || f.type->isStringLiteral();
    };
    if (llvm::all_of(entry.fields, [&](llvm::StringRef f) { return type->field(f) != nullptr; }) &&
        llvm::all_of(type->fields(), listed)) {
      return entry.fields;
    }
  }
  llvm_unreachable("no maker for this type");
}

bool ObjectMaker::isNestedPart(const Type *type) {
  return type->isObject() && !type->isClass();
}

std::vector<const Type *> ObjectMaker::literalsOf(const Type *type) {
  if (type->isStringLiteral()) {
    return {type};
  }

  return std::vector<const Type *>(type->members().begin(), type->members().end());
}

ObjectMaker::ObjectMaker(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ObjectMaker::atomOf(const Type *type, llvm::Value *text) {
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::FunctionCallee compare = this->lowering.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  std::vector<const Type *> literals = ObjectMaker::literalsOf(type);
  llvm::Value *atom =
      this->lowering.builder_.getInt32(literals.empty() ? 0 : literals.front()->atom());
  if (literals.size() == 1) {
    return atom;
  }
  for (const Type *literal : literals) {
    std::string ascii;
    for (char16_t unit : literal->literalText()) {
      ascii.push_back(char(unit));
    }
    llvm::Value *same = this->lowering.builder_.CreateICmpEQ(
        this->lowering.builder_.CreateCall(
            compare, {text, this->lowering.stringLowerer().utf8Constant(ascii)}),
        this->lowering.builder_.getInt32(0));
    atom = this->lowering.builder_.CreateSelect(
        same, this->lowering.builder_.getInt32(literal->atom()), atom);
  }

  return atom;
}

llvm::StructType *ObjectMaker::parts(const Type *type, llvm::ArrayRef<MakerFields> orders) {
  std::vector<llvm::Type *> elements;
  for (llvm::StringRef name : ObjectMaker::orderOf(type, orders)) {
    const Type *field = type->field(name)->type;
    elements.push_back(ObjectMaker::isNestedPart(field) ? this->parts(field, orders)
                       : UnionLayout::isAtomType(field)
                           ? this->lowering.builder_.getPtrTy()
                           : this->lowering.moduleBuilder().lower(field));
  }

  return llvm::StructType::get(this->lowering.ctx_, elements);
}

llvm::Function *ObjectMaker::maker(const Type *type, llvm::ArrayRef<MakerFields> orders) {
  llvm::Function *&made = this->makers[type];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  std::string name = cls                             ? cls->name.str()
                     : type->name().starts_with("{") ? "object"
                                                     : type->name().str();
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make." + name,
                                this->lowering.module_.get());
  llvm::Function *function = made;
  this->lowering.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function_, function);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", function));

  const std::vector<llvm::StringRef> &order = ObjectMaker::orderOf(type, orders);
  llvm::Value *object;
  if (cls) {
    object = this->lowering.classLowerer().allocInstance(cls);
  } else {
    std::vector<unsigned> keys;
    for (const Field &field : type->fields()) {
      if (!llvm::is_contained(order, field.name)) {
        keys.push_back(ObjectLayouts::fieldIndex(type, field.name));
      }
    }
    for (llvm::StringRef field : order) {
      keys.push_back(ObjectLayouts::fieldIndex(type, field));
    }
    object = this->lowering.objectLayouts().allocObject(type, keys);
  }
  llvm::StructType *layout = this->parts(type, orders);
  for (size_t i = 0; i < order.size(); ++i) {
    const Type *field = type->field(order[i])->type;
    llvm::Value *address =
        this->lowering.builder_.CreateStructGEP(layout, function->getArg(0), unsigned(i));
    llvm::Value *value =
        ObjectMaker::isNestedPart(field)
            ? static_cast<llvm::Value *>(
                  this->lowering.builder_.CreateCall(this->maker(field, orders), {address}))
        : UnionLayout::isAtomType(field)
            ? this->atomOf(field, this->lowering.builder_.CreateLoad(ptr, address, order[i]))
            : this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(field),
                                                 address, order[i]);
    this->lowering.builder_.CreateStore(
        value, this->lowering.objectLayouts().fieldAddress(object, type, order[i], true));
  }
  for (const Field &field : type->fields()) {
    if (!llvm::is_contained(order, field.name)) {
      this->lowering.builder_.CreateStore(
          this->lowering.builder_.getInt32(field.type->atom()),
          this->lowering.objectLayouts().fieldAddress(object, type, field.name, true));
    }
  }
  this->lowering.groupLowerer().adoptFields(object, type);
  this->lowering.builder_.CreateRet(object);
  this->lowering.function_ = outer;
  this->lowering.builder_.restoreIP(point);

  return function;
}

}
