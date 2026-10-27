#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

const char16_t *const kKindNames[] = {u"undefined", u"number", u"string",
                                      u"boolean",   u"object", u"function"};

constexpr uint32_t kLayoutClass = 2097152;

constexpr uint32_t kLayoutBuiltin = 4194304;

constexpr int64_t kStaticRefCount = int64_t(1) << 62;

bool hasCall(const Expr *expr) {
  if (!expr) {
    return false;
  }
  if (isa<CallExpr>(expr)) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return hasCall(member->object);
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return hasCall(index->object) || hasCall(index->index);
  }
  if (auto *conv = dyn_cast<CastExpr>(expr)) {
    return hasCall(conv->operand);
  }

  return false;
}

bool isObjectHeaded(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

}

UnknownLowerer::UnknownLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee UnknownLowerer::unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *UnknownLowerer::boxValue(llvm::Value *value, const Type *type) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  if (type->isFunction()) {
    lowering_.unknownFunctionTypes_.insert(type);
  }
  const char *box =
      lowering_.memory().mayHoldMember(type) ? "tz_unknown_box_member" : "tz_unknown_box";

  return lowering_.builder_.CreateCall(unknownFunction(box, ptr, {ptr, ptr}),
                                       {lowering_.typeDescriptors().typeDescriptor(type),
                                        lowering_.functionFrame().spill(value, "box.value")},
                                       "box");
}

llvm::Value *UnknownLowerer::emitUnknownConversion(llvm::Value *value, const Type *from,
                                                   const Type *to) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  if (to->isUnknown()) {
    if (from->isUnknown() || isObjectHeaded(from)) {
      return value;
    }
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    }
    if (from->nonOptional()->isUnion() || from->nonOptional()->isLiteral()) {
      return lowering_.builder_.CreateCall(unknownFunction("tz_unknown_of", ptr, {ptr, ptr}),
                                           {lowering_.typeDescriptors().typeDescriptor(from),
                                            lowering_.functionFrame().spill(value, "union.value")},
                                           "unknown");
    }
    if (!from->isOptional()) {
      return boxValue(value, from);
    }
    const Type *inner = from->inner();
    if (isObjectHeaded(inner)) {
      return value;
    }
    llvm::BasicBlock *boxBlock = lowering_.functionFrame().newBlock("unknown.box");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("unknown.end");
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value, from), boxBlock,
                                    endBlock);
    lowering_.functionFrame().switchTo(boxBlock);
    llvm::Value *box =
        emitUnknownConversion(lowering_.optionalLowerer().optionalValue(value, from), inner, to);
    llvm::BasicBlock *boxEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(ptr, 2);
    phi->addIncoming(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), entry);
    phi->addIncoming(box, boxEnd);
    return phi;
  }
  llvm::Value *result;
  if (to->isString()) {
    result = lowering_.stringLowerer().callReturningString(
        unknownFunction("tz_unknown_string", lowering_.builder_.getVoidTy(), {ptr, ptr}), {value});
  } else if (to->isFloat()) {
    result = lowering_.builder_.CreateCall(
        unknownFunction("tz_unknown_number", lowering_.builder_.getDoubleTy(), {ptr}), {value});
  } else if (to->isBool()) {
    llvm::Value *flag = lowering_.builder_.CreateCall(
        unknownFunction("tz_unknown_bool", lowering_.builder_.getInt32Ty(), {ptr}), {value});
    result = lowering_.builder_.CreateICmpNE(flag, lowering_.builder_.getInt32(0));
  } else if (to->isArray()) {
    result = lowering_.builder_.CreateCall(unknownFunction("tz_unknown_array", ptr, {ptr, ptr}),
                                           {value, lowering_.typeDescriptors().typeDescriptor(to)});
  } else {
    return value;
  }
  lowering_.refCounts().releaseValue(value, from);

  return result;
}

llvm::Value *UnknownLowerer::emitUnknownTypeOf(Expr *operand) {
  Lowering::Operand value = lowering_.refCounts().emitOperand(operand, true);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *kind = lowering_.builder_.CreateCall(
      unknownFunction("tz_unknown_kind", lowering_.builder_.getInt32Ty(), {ptr}), {value.value},
      "kind");
  lowering_.refCounts().dropOperand(value);
  llvm::Value *text = lowering_.stringLowerer().stringLiteral(kKindNames[0]);
  for (unsigned k = 1; k < std::size(kKindNames); ++k) {
    text = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpEQ(kind, lowering_.builder_.getInt32(k)),
        lowering_.stringLowerer().stringLiteral(kKindNames[k]), text);
  }

  return text;
}

llvm::Value *UnknownLowerer::emitUnknownIsArray(Expr *arg) {
  Lowering::Operand value = lowering_.refCounts().emitOperand(arg, true);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *array = lowering_.builder_.CreateCall(
      unknownFunction("tz_unknown_is_array", lowering_.builder_.getInt32Ty(), {ptr}),
      {value.value});
  lowering_.refCounts().dropOperand(value);

  return lowering_.builder_.CreateICmpNE(array, lowering_.builder_.getInt32(0));
}

llvm::Constant *UnknownLowerer::classObject(const ClassDecl *cls) {
  if (llvm::Constant *found = classObjects_.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  std::vector<const ClassField *> statics;
  std::vector<llvm::Type *> elements{i64, ptr, ptr, i32, i32, ptr};
  if (cls->usedAsValue) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.var) {
        statics.push_back(&f);
        elements.push_back(lowering_.moduleBuilder().lower(f.var->type));
      }
    }
  }
  std::string name = lowering_.symbolPrefixes_.lookup(cls) + cls->name.str();
  auto *type = llvm::StructType::create(lowering_.ctx_, elements, "tz.class." + name);
  classObjectTypes_[cls] = type;
  llvm::Constant *parent = cls->parent
                               ? classObject(cls->parent)
                               : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  auto *drop = cast<llvm::Function>(
      unknownFunction("tz_static_drop", lowering_.builder_.getVoidTy(), {ptr}).getCallee());
  const llvm::StructLayout *offsets = lowering_.module_->getDataLayout().getStructLayout(type);
  std::vector<llvm::Constant *> keys;
  for (size_t i = 0; i < statics.size(); ++i) {
    llvm::StringRef key = statics[i]->name;
    if (key.starts_with("#")) {
      continue;
    }
    keys.push_back(llvm::ConstantStruct::get(
        lowering_.fieldDescTy_,
        {lowering_.stringLowerer().utf8Constant(key),
         lowering_.builder_.getInt32(uint32_t(key.size())),
         lowering_.builder_.getInt32(uint32_t(offsets->getElementOffset(unsigned(6 + i)))),
         lowering_.typeDescriptors().typeDescriptor(statics[i]->var->type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (!keys.empty()) {
    auto *arrayType = llvm::ArrayType::get(lowering_.fieldDescTy_, keys.size());
    auto *global = new llvm::GlobalVariable(
        *lowering_.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, keys), "tz.classfields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  uint32_t flags = kLayoutClass | (cls->builtin || hasCall(cls->baseExpr) ? kLayoutBuiltin : 0);
  llvm::Constant *layoutInit = llvm::ConstantStruct::get(
      lowering_.layoutDescTy_,
      {drop, fields, lowering_.builder_.getInt32(uint32_t(keys.size())),
       lowering_.builder_.getInt32(flags), lowering_.stringLowerer().utf8Constant(cls->name),
       lowering_.builder_.getInt32(uint32_t(cls->name.size())),
       lowering_.builder_.getInt32(
           lowering_.unionLowerer().plainTypeId(Type::classRef(cls, cls->name)))});
  auto *layout = new llvm::GlobalVariable(*lowering_.module_, lowering_.layoutDescTy_, true,
                                          llvm::GlobalValue::PrivateLinkage, layoutInit,
                                          "tz.classlayout." + name);
  layout->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Constant *info = (cls->reflectNew || cls->reflectMethods) && !cls->builtin
                             ? lowering_.classInfoLowerer().classInfoGlobal(cls)
                             : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  std::vector<llvm::Constant *> init{lowering_.builder_.getInt64(kStaticRefCount),
                                     layout,
                                     parent,
                                     lowering_.builder_.getInt32(cls->id),
                                     lowering_.builder_.getInt32(std::max(cls->id, cls->lastId)),
                                     info};
  for (size_t i = 6; i < elements.size(); ++i) {
    init.push_back(llvm::Constant::getNullValue(elements[i]));
  }
  auto *object =
      new llvm::GlobalVariable(*lowering_.module_, type, false, llvm::GlobalValue::InternalLinkage,
                               llvm::ConstantStruct::get(type, init), "tz.class." + name);
  classObjects_[cls] = object;

  return object;
}

llvm::Constant *UnknownLowerer::staticFieldAddress(const ClassDecl *cls, const VarSymbol *var) {
  llvm::Constant *object = classObject(cls);
  unsigned element = 6;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic && f.var) {
      if (f.var == var) {
        break;
      }
      ++element;
    }
  }

  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      classObjectTypes_.lookup(cls), object,
      llvm::ArrayRef<llvm::Constant *>{lowering_.builder_.getInt32(0),
                                       lowering_.builder_.getInt32(element)});
}

llvm::Value *UnknownLowerer::emitTupleJoin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *tuple = member->object->type;
  bool hasSeparator = !call->args.empty();
  Lowering::Operand object = lowering_.refCounts().emitOperand(
      member->object, !hasSeparator || AssignLowerer::cannotAssign(call->args[0]));
  Lowering::Operand separator =
      hasSeparator
          ? lowering_.refCounts().emitOperand(call->args[0], true)
          : Lowering::Operand{lowering_.stringLowerer().stringLiteral(u","), false, Type::string()};
  std::vector<Lowering::Operand> parts;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    if (i > 0) {
      parts.push_back({separator.value, false, Type::string()});
    }
    const Type *type = tuple->fields()[i].type;
    llvm::Value *element = lowering_.builder_.CreateLoad(
        lowering_.moduleBuilder().lower(type),
        lowering_.objectLayouts().fieldAddress(object.value, tuple, std::to_string(i)), "element");
    lowering_.refCounts().retainValue(element, type);
    llvm::Value *text;
    if (type->isUndefined()) {
      text = lowering_.stringLowerer().stringLiteral(u"");
    } else if (type->isOptional()) {
      llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("join.value");
      llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("join.end");
      llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(element, type),
                                      valueBlock, endBlock);
      lowering_.functionFrame().switchTo(valueBlock);
      llvm::Value *shown = lowering_.exprLowerer().emitConversion(
          lowering_.optionalLowerer().optionalValue(element, type), type->inner(), Type::string());
      llvm::BasicBlock *valueEnd = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateBr(endBlock);
      lowering_.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.strTy_, 2);
      phi->addIncoming(lowering_.stringLowerer().stringLiteral(u""), entry);
      phi->addIncoming(shown, valueEnd);
      text = phi;
    } else {
      text = lowering_.exprLowerer().emitConversion(element, type, Type::string());
    }
    parts.push_back({text, true, Type::string()});
  }
  llvm::Value *result = parts.empty() ? lowering_.stringLowerer().stringLiteral(u"")
                                      : lowering_.stringLowerer().concatOperands(parts);
  lowering_.refCounts().dropOperand(separator);
  lowering_.refCounts().dropOperand(object);

  return result;
}

}
