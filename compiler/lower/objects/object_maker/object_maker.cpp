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

namespace {

const std::vector<llvm::StringRef> &orderOf(const Type *type, llvm::ArrayRef<MakerFields> orders) {
  for (const MakerFields &entry : orders) {
    if (const ClassDecl *cls = type->classDecl()) {
      if (entry.cls && cls->name == entry.cls) {
        return entry.fields;
      }
      continue;
    }
    if (entry.cls || type->fields().size() != entry.fields.size()) {
      continue;
    }
    if (llvm::all_of(entry.fields, [&](llvm::StringRef f) { return type->field(f) != nullptr; })) {
      return entry.fields;
    }
  }
  llvm_unreachable("no maker for this type");
}

bool isNestedPart(const Type *type) { return type->isObject() && !type->isClass(); }

std::vector<const Type *> literalsOf(const Type *type) {
  if (type->isStringLiteral()) {
    return {type};
  }

  return std::vector<const Type *>(type->members().begin(), type->members().end());
}

}

ObjectMaker::ObjectMaker(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ObjectMaker::atomOf(const Type *type, llvm::Value *text) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *ptr = lowering_.builder_.getPtrTy();
  llvm::FunctionCallee compare = lowering_.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  llvm::Value *atom = lowering_.builder_.getInt32(0);
  for (const Type *literal : literalsOf(type)) {
    std::string ascii;
    for (char16_t unit : literal->literalText()) {
      ascii.push_back(char(unit));
    }
    llvm::Value *same = lowering_.builder_.CreateICmpEQ(
        lowering_.builder_.CreateCall(compare,
                                      {text, lowering_.stringLowerer().utf8Constant(ascii)}),
        lowering_.builder_.getInt32(0));
    atom =
        lowering_.builder_.CreateSelect(same, lowering_.builder_.getInt32(literal->atom()), atom);
  }

  return atom;
}

llvm::StructType *ObjectMaker::parts(const Type *type, llvm::ArrayRef<MakerFields> orders) {
  std::vector<llvm::Type *> elements;
  for (llvm::StringRef name : orderOf(type, orders)) {
    const Type *field = type->field(name)->type;
    elements.push_back(isNestedPart(field)              ? parts(field, orders)
                       : UnionLayout::isAtomType(field) ? lowering_.builder_.getPtrTy()
                                                        : lowering_.moduleBuilder().lower(field));
  }

  return llvm::StructType::get(lowering_.ctx_, elements);
}

llvm::Function *ObjectMaker::maker(const Type *type, llvm::ArrayRef<MakerFields> orders) {
  llvm::Function *&made = makers_[type];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  std::string name = cls                             ? cls->name.str()
                     : type->name().starts_with("{") ? "object"
                                                     : type->name().str();
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make." + name,
                                lowering_.module_.get());
  llvm::Function *function = made;
  lowering_.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, function);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", function));

  const std::vector<llvm::StringRef> &order = orderOf(type, orders);
  llvm::Value *object;
  if (cls) {
    object = lowering_.classLowerer().allocInstance(cls);
  } else {
    std::vector<unsigned> keys;
    for (llvm::StringRef field : order) {
      keys.push_back(ObjectLayouts::fieldIndex(type, field));
    }
    object = lowering_.objectLayouts().allocObject(type, keys);
  }
  llvm::StructType *layout = parts(type, orders);
  for (size_t i = 0; i < order.size(); ++i) {
    const Type *field = type->field(order[i])->type;
    llvm::Value *address =
        lowering_.builder_.CreateStructGEP(layout, function->getArg(0), unsigned(i));
    llvm::Value *value = isNestedPart(field)
                             ? static_cast<llvm::Value *>(
                                   lowering_.builder_.CreateCall(maker(field, orders), {address}))
                         : UnionLayout::isAtomType(field)
                             ? atomOf(field, lowering_.builder_.CreateLoad(ptr, address, order[i]))
                             : lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(field),
                                                             address, order[i]);
    lowering_.builder_.CreateStore(
        value, lowering_.objectLayouts().fieldAddress(object, type, order[i], true));
  }
  lowering_.groupLowerer().adoptFields(object, type);
  lowering_.builder_.CreateRet(object);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return function;
}

}
