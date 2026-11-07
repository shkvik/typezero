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
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr uint32_t kLayoutClass = 2097152;

constexpr uint32_t kLayoutBuiltin = 4194304;

constexpr int64_t kStaticRefCount = int64_t(1) << 62;

}

bool UnknownLowerer::hasCall(const Expr *expr) {
  if (!expr) {
    return false;
  }
  if (isa<CallExpr>(expr)) {
    return true;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member) {
    return UnknownLowerer::hasCall(member->object);
  }
  auto *index = dyn_cast<IndexExpr>(expr);
  if (index) {
    return UnknownLowerer::hasCall(index->object) || UnknownLowerer::hasCall(index->index);
  }
  auto *conv = dyn_cast<CastExpr>(expr);
  if (conv) {
    return UnknownLowerer::hasCall(conv->operand);
  }

  return false;
}

bool UnknownLowerer::isObjectHeaded(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

UnknownLowerer::UnknownLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee UnknownLowerer::unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *UnknownLowerer::boxValue(llvm::Value *value, const Type *type) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  if (type->isFunction()) {
    this->lowering.unknownFunctionTypes_.insert(type);
  }
  if (type->isJson()) {
    return this->lowering.builder_.CreateCall(
        this->unknownFunction("tz_unknown_json", ptr,
                              {ptr, ptr, this->lowering.builder_.getInt32Ty()}),
        {this->lowering.typeDescriptors().typeDescriptor(type), value,
         this->lowering.builder_.getInt32(this->lowering.memory().mayCarryMember(type) ? 1 : 0)},
        "box");
  }
  const char *box =
      this->lowering.memory().mayCarryMember(type) ? "tz_unknown_box_member" : "tz_unknown_box";

  return this->lowering.builder_.CreateCall(
      this->unknownFunction(box, ptr, {ptr, ptr}),
      {this->lowering.typeDescriptors().typeDescriptor(type),
       this->lowering.functionFrame().spill(value, "box.value")},
      "box");
}

llvm::Value *UnknownLowerer::emitUnknownConversion(llvm::Value *value, const Type *from,
                                                   const Type *to) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  if (to->isUnknown()) {
    if (from->isUnknown() || UnknownLowerer::isObjectHeaded(from)) {
      return value;
    }
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
    }
    if (from->nonOptional()->isUnion() || from->nonOptional()->isLiteral()) {
      return this->lowering.builder_.CreateCall(
          this->unknownFunction("tz_unknown_of", ptr, {ptr, ptr}),
          {this->lowering.typeDescriptors().typeDescriptor(from),
           this->lowering.functionFrame().spill(value, "union.value")},
          "unknown");
    }
    if (!from->isOptional()) {
      return this->boxValue(value, from);
    }
    const Type *inner = from->inner();
    if (UnknownLowerer::isObjectHeaded(inner)) {
      return value;
    }
    llvm::BasicBlock *boxBlock = this->lowering.functionFrame().newBlock("unknown.box");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("unknown.end");
    llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
    this->lowering.builder_.CreateCondBr(this->lowering.optionalLowerer().optionalHas(value, from),
                                         boxBlock, endBlock);
    this->lowering.functionFrame().switchTo(boxBlock);
    llvm::Value *box = this->emitUnknownConversion(
        this->lowering.optionalLowerer().optionalValue(value, from), inner, to);
    llvm::BasicBlock *boxEnd = this->lowering.builder_.GetInsertBlock();
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder_.CreatePHI(ptr, 2);
    phi->addIncoming(llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), entry);
    phi->addIncoming(box, boxEnd);
    return phi;
  }
  llvm::Value *result;
  if (to->isString()) {
    result = this->lowering.stringLowerer().callReturningString(
        this->unknownFunction("tz_unknown_string", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {value});
  } else if (to->isFloat()) {
    result = this->lowering.builder_.CreateCall(
        this->unknownFunction("tz_unknown_number", this->lowering.builder_.getDoubleTy(), {ptr}),
        {value});
  } else if (to->isBool()) {
    llvm::Value *flag = this->lowering.builder_.CreateCall(
        this->unknownFunction("tz_unknown_bool", this->lowering.builder_.getInt32Ty(), {ptr}),
        {value});
    result = this->lowering.builder_.CreateICmpNE(flag, this->lowering.builder_.getInt32(0));
  } else if (to->isArray()) {
    result = this->lowering.builder_.CreateCall(
        this->unknownFunction("tz_unknown_array", ptr, {ptr, ptr}),
        {value, this->lowering.typeDescriptors().typeDescriptor(to)});
  } else if (to->isInteger() || to->isDecimal() || to->isJson()) {
    llvm::Type *lowered = this->lowering.moduleBuilder().lower(to);
    llvm::AllocaInst *slot = this->lowering.functionFrame().createStackSlot(lowered, "unboxed");
    this->lowering.builder_.CreateCall(
        this->unknownFunction("tz_unknown_unbox", this->lowering.builder_.getVoidTy(),
                              {ptr, ptr, ptr}),
        {value, this->lowering.typeDescriptors().typeDescriptor(to), slot});
    result = this->lowering.builder_.CreateLoad(lowered, slot, "unboxed");
  } else {
    return value;
  }
  this->lowering.refCounts().releaseValue(value, from);

  return result;
}

llvm::Value *UnknownLowerer::typeOfText(llvm::Value *code) {
  auto text16 = [](llvm::StringRef name) { return std::u16string(name.begin(), name.end()); };
  llvm::Value *text = this->lowering.stringLowerer().stringLiteral(text16(kTypeOfNames[0]));
  for (unsigned k = 1; k < std::size(kTypeOfNames); ++k) {
    text = this->lowering.builder_.CreateSelect(
        this->lowering.builder_.CreateICmpEQ(code, this->lowering.builder_.getInt32(k)),
        this->lowering.stringLowerer().stringLiteral(text16(kTypeOfNames[k])), text);
  }

  return text;
}

llvm::Value *UnknownLowerer::emitUnknownTypeOf(Expr *operand) {
  Lowering::Operand value = this->lowering.refCounts().emitOperand(operand, true);
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *code = this->lowering.builder_.CreateCall(
      this->unknownFunction("tz_unknown_typeof", this->lowering.builder_.getInt32Ty(), {ptr}),
      {value.value}, "typeof");
  this->lowering.refCounts().dropOperand(value);

  return this->typeOfText(code);
}

llvm::Value *UnknownLowerer::emitUnknownIsJson(Expr *arg) {
  Lowering::Operand value = this->lowering.refCounts().emitOperand(arg, true);
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *json = this->lowering.builder_.CreateCall(
      this->unknownFunction("tz_unknown_is_json", this->lowering.builder_.getInt32Ty(), {ptr}),
      {value.value});
  this->lowering.refCounts().dropOperand(value);

  return this->lowering.builder_.CreateICmpNE(json, this->lowering.builder_.getInt32(0));
}

llvm::Value *UnknownLowerer::emitUnknownIsArray(Expr *arg) {
  Lowering::Operand value = this->lowering.refCounts().emitOperand(arg, true);
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *array = this->lowering.builder_.CreateCall(
      this->unknownFunction("tz_unknown_is_array", this->lowering.builder_.getInt32Ty(), {ptr}),
      {value.value});
  this->lowering.refCounts().dropOperand(value);

  return this->lowering.builder_.CreateICmpNE(array, this->lowering.builder_.getInt32(0));
}

llvm::Constant *UnknownLowerer::classObject(const ClassDecl *cls) {
  llvm::Constant *found = this->classObjects.lookup(cls);
  if (found) {
    return found;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  std::vector<const ClassField *> statics;
  std::vector<llvm::Type *> elements{i64, ptr, ptr, i32, i32, ptr};
  if (cls->usedAsValue) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.var()) {
        statics.push_back(&f);
        elements.push_back(this->lowering.moduleBuilder().lower(f.var()->type));
      }
    }
  }
  std::string name = this->lowering.symbolPrefixes_.lookup(cls) + cls->name.str();
  auto *type = llvm::StructType::create(this->lowering.ctx_, elements, "tz.class." + name);
  this->classObjectTypes[cls] = type;
  llvm::Constant *parent = cls->parent
                               ? this->classObject(cls->parent)
                               : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  auto *drop = cast<llvm::Function>(
      this->unknownFunction("tz_static_drop", this->lowering.builder_.getVoidTy(), {ptr})
          .getCallee());
  const llvm::StructLayout *offsets = this->lowering.module_->getDataLayout().getStructLayout(type);
  std::vector<llvm::Constant *> keys;
  for (size_t i = 0; i < statics.size(); ++i) {
    llvm::StringRef key = statics[i]->name;
    keys.push_back(llvm::ConstantStruct::get(
        this->lowering.fieldDescTy_,
        {this->lowering.stringLowerer().utf8Constant(key),
         this->lowering.builder_.getInt32(uint32_t(key.size())),
         this->lowering.builder_.getInt32(uint32_t(offsets->getElementOffset(unsigned(6 + i)))),
         this->lowering.typeDescriptors().typeDescriptor(statics[i]->var()->type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  if (!keys.empty()) {
    auto *arrayType = llvm::ArrayType::get(this->lowering.fieldDescTy_, keys.size());
    auto *global = new llvm::GlobalVariable(
        *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, keys), "tz.classfields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  uint32_t flags = kLayoutClass |
                   (cls->builtin() || UnknownLowerer::hasCall(cls->baseExpr) ? kLayoutBuiltin : 0);
  llvm::Constant *layoutInit = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy_,
      {drop, fields, this->lowering.builder_.getInt32(uint32_t(keys.size())),
       this->lowering.builder_.getInt32(flags),
       this->lowering.stringLowerer().utf8Constant(cls->name),
       this->lowering.builder_.getInt32(uint32_t(cls->name.size())),
       this->lowering.builder_.getInt32(
           this->lowering.unionLowerer().plainTypeId(Type::classRef(cls, cls->name)))});
  auto *layout = new llvm::GlobalVariable(*this->lowering.module_, this->lowering.layoutDescTy_,
                                          true, llvm::GlobalValue::PrivateLinkage, layoutInit,
                                          "tz.classlayout." + name);
  layout->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Constant *info =
      (cls->reflectNew || cls->reflectMethods || cls->reflectFill) && !cls->builtin()
          ? this->lowering.classInfoLowerer().classInfoGlobal(cls)
          : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  std::vector<llvm::Constant *> init{
      this->lowering.builder_.getInt64(kStaticRefCount),
      layout,
      parent,
      this->lowering.builder_.getInt32(cls->id),
      this->lowering.builder_.getInt32(std::max(cls->id, cls->lastId)),
      info};
  for (size_t i = 6; i < elements.size(); ++i) {
    init.push_back(llvm::Constant::getNullValue(elements[i]));
  }
  auto *object = new llvm::GlobalVariable(
      *this->lowering.module_, type, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantStruct::get(type, init), "tz.class." + name);
  this->classObjects[cls] = object;

  return object;
}

llvm::Constant *UnknownLowerer::staticFieldAddress(const ClassDecl *cls, const VarSymbol *var) {
  llvm::Constant *object = this->classObject(cls);
  unsigned element = 6;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic() && f.var()) {
      if (f.var() == var) {
        break;
      }
      ++element;
    }
  }

  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      this->classObjectTypes.lookup(cls), object,
      llvm::ArrayRef<llvm::Constant *>{this->lowering.builder_.getInt32(0),
                                       this->lowering.builder_.getInt32(element)});
}

llvm::Value *UnknownLowerer::emitTupleJoin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *tuple = member->object->type;
  bool hasSeparator = !call->args.empty();
  Lowering::Operand object = this->lowering.refCounts().emitOperand(
      member->object, !hasSeparator || AssignLowerer::cannotAssign(call->args[0]));
  Lowering::Operand separator =
      hasSeparator ? this->lowering.refCounts().emitOperand(call->args[0], true)
                   : Lowering::Operand{this->lowering.stringLowerer().stringLiteral(u","), false,
                                       Type::string()};
  std::vector<Lowering::Operand> parts;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    if (i > 0) {
      parts.push_back({separator.value, false, Type::string()});
    }
    const Type *type = tuple->fields()[i].type;
    llvm::Value *element = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(type),
        this->lowering.objectLayouts().fieldAddress(object.value, tuple, std::to_string(i)),
        "element");
    this->lowering.refCounts().retainValue(element, type);
    llvm::Value *text;
    if (type->isUndefined()) {
      text = this->lowering.stringLowerer().stringLiteral(u"");
    } else if (type->isOptional()) {
      llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("join.value");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("join.end");
      llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateCondBr(
          this->lowering.optionalLowerer().optionalHas(element, type), valueBlock, endBlock);
      this->lowering.functionFrame().switchTo(valueBlock);
      llvm::Value *shown = this->lowering.exprLowerer().emitConversion(
          this->lowering.optionalLowerer().optionalValue(element, type), type->inner(),
          Type::string());
      llvm::BasicBlock *valueEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = this->lowering.builder_.CreatePHI(this->lowering.strTy_, 2);
      phi->addIncoming(this->lowering.stringLowerer().stringLiteral(u""), entry);
      phi->addIncoming(shown, valueEnd);
      text = phi;
    } else {
      text = this->lowering.exprLowerer().emitConversion(element, type, Type::string());
    }
    parts.push_back({text, true, Type::string()});
  }
  llvm::Value *result = parts.empty() ? this->lowering.stringLowerer().stringLiteral(u"")
                                      : this->lowering.stringLowerer().concatOperands(parts);
  this->lowering.refCounts().dropOperand(separator);
  this->lowering.refCounts().dropOperand(object);

  return result;
}

}
