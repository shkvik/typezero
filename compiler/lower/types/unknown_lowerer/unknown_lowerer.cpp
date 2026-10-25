#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// What tz_unknown_kind gives, TZ_KIND_* of runtime/core/tzrt/tzrt.h: the texts
// of typeof.
const char16_t *const kKindNames[] = {u"undefined", u"number", u"string",
                                      u"boolean",   u"object", u"function"};

// TZ_LAYOUT_CLASS and TZ_LAYOUT_BUILTIN of runtime/core/tzrt/tzrt.h.
constexpr uint32_t kLayoutClass = 2097152;

constexpr uint32_t kLayoutBuiltin = 4194304;

// A count no program reaches, as literals have (see codegen.cpp).
constexpr int64_t kStaticRefCount = int64_t(1) << 62;

// Whether evaluating `expr` calls something, as `Mixin(Base)` does.
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

// A reference to an object of the language, which an unknown holds as it is.
bool isObjectHeaded(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

UnknownLowerer::UnknownLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee UnknownLowerer::unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                              llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *UnknownLowerer::boxValue(llvm::Value *value, const Type *type) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  // Reflect.apply may call it (see finishReflection).
  if (type->isFunction()) {
    codegen_.unknownFunctionTypes_.insert(type);
  }
  // A value that may hold a member of a group goes into a box that is one
  // (runtime/core/unknown/unknown.c).
  const char *box = codegen_.memoryAnalysis().mayHoldMember(type) ? "tz_unknown_box_member" : "tz_unknown_box";

  return codegen_.builder_.CreateCall(unknownFunction(box, ptr, {ptr, ptr}),
                             {codegen_.typeDescriptors().typeDescriptor(type), codegen_.functionFrame().spill(value, "box.value")}, "box");
}

llvm::Value *UnknownLowerer::emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  if (to->isUnknown()) {
    if (from->isUnknown() || isObjectHeaded(from)) {
      return value;
    }
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
    }
    // A union or a literal type: what it holds, as tz_unknown_of finds it
    // (a reference as it is, any other value in a box of its own type; a
    // string literal as its string).
    if (from->nonOptional()->isUnion() || from->nonOptional()->isLiteral()) {
      return codegen_.builder_.CreateCall(unknownFunction("tz_unknown_of", ptr, {ptr, ptr}),
                                 {codegen_.typeDescriptors().typeDescriptor(from), codegen_.functionFrame().spill(value, "union.value")}, "unknown");
    }
    if (!from->isOptional()) {
      return boxValue(value, from);
    }
    const Type *inner = from->inner();
    if (isObjectHeaded(inner)) { // null for undefined
      return value;
    }
    // A value in a box, or null for undefined.
    llvm::BasicBlock *boxBlock = codegen_.functionFrame().newBlock("unknown.box");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("unknown.end");
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value, from), boxBlock, endBlock);
    codegen_.functionFrame().switchTo(boxBlock);
    llvm::Value *box = emitUnknownConversion(codegen_.optionalLowerer().optionalValue(value, from), inner, to);
    llvm::BasicBlock *boxEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(ptr, 2);
    phi->addIncoming(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), entry);
    phi->addIncoming(box, boxEnd);
    return phi;
  }
  // What a check showed: the value in the box, or the reference itself.
  llvm::Value *result;
  if (to->isString()) {
    result = codegen_.stringLowerer().callReturningString(
        unknownFunction("tz_unknown_string", codegen_.builder_.getVoidTy(), {ptr, ptr}), {value});
  } else if (to->isFloat()) {
    result = codegen_.builder_.CreateCall(
        unknownFunction("tz_unknown_number", codegen_.builder_.getDoubleTy(), {ptr}), {value});
  } else if (to->isBool()) {
    llvm::Value *flag = codegen_.builder_.CreateCall(
        unknownFunction("tz_unknown_bool", codegen_.builder_.getInt32Ty(), {ptr}), {value});
    result = codegen_.builder_.CreateICmpNE(flag, codegen_.builder_.getInt32(0));
  } else if (to->isArray()) {
    result = codegen_.builder_.CreateCall(unknownFunction("tz_unknown_array", ptr, {ptr, ptr}),
                                 {value, codegen_.typeDescriptors().typeDescriptor(to)});
  } else {
    return value; // an object, of a class or any
  }
  codegen_.refCounts().releaseValue(value, from);

  return result;
}

llvm::Value *UnknownLowerer::emitUnknownTypeOf(Expr *operand) {
  CodeGen::Operand value = codegen_.refCounts().emitOperand(operand, /*canBorrow=*/true);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *kind = codegen_.builder_.CreateCall(
      unknownFunction("tz_unknown_kind", codegen_.builder_.getInt32Ty(), {ptr}), {value.value}, "kind");
  codegen_.refCounts().dropOperand(value);
  llvm::Value *text = codegen_.stringLowerer().stringLiteral(kKindNames[0]);
  for (unsigned k = 1; k < std::size(kKindNames); ++k) {
    text = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpEQ(kind, codegen_.builder_.getInt32(k)),
                                 codegen_.stringLowerer().stringLiteral(kKindNames[k]), text);
  }

  return text;
}

llvm::Value *UnknownLowerer::emitUnknownIsArray(Expr *arg) {
  CodeGen::Operand value = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *array = codegen_.builder_.CreateCall(
      unknownFunction("tz_unknown_is_array", codegen_.builder_.getInt32Ty(), {ptr}), {value.value});
  codegen_.refCounts().dropOperand(value);

  return codegen_.builder_.CreateICmpNE(array, codegen_.builder_.getInt32(0));
}

// { TzClass, the static fields }: the static fields of a class used as a
// value live in its object, whose layout describes those that are keys, so
// that it prints as node prints a class, `[class User] { count: 0 }`.
llvm::Constant *UnknownLowerer::classObject(const ClassDecl *cls) {
  if (llvm::Constant *found = classObjects_.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  std::vector<const ClassField *> statics;
  std::vector<llvm::Type *> elements{i64, ptr, ptr, i32, i32, ptr};
  if (cls->usedAsValue) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.var) {
        statics.push_back(&f);
        elements.push_back(codegen_.moduleBuilder().lower(f.var->type));
      }
    }
  }
  std::string name = codegen_.symbolPrefixes_.lookup(cls) + cls->name.str();
  auto *type = llvm::StructType::create(codegen_.ctx_, elements, "tz.class." + name);
  classObjectTypes_[cls] = type;
  llvm::Constant *parent =
      cls->parent ? classObject(cls->parent) : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  auto *drop = cast<llvm::Function>(
      unknownFunction("tz_static_drop", codegen_.builder_.getVoidTy(), {ptr}).getCallee());
  // The keys: the static fields but the `#private` ones, as node shows them.
  const llvm::StructLayout *offsets = codegen_.module_->getDataLayout().getStructLayout(type);
  std::vector<llvm::Constant *> keys;
  for (size_t i = 0; i < statics.size(); ++i) {
    llvm::StringRef key = statics[i]->name;
    if (key.starts_with("#")) {
      continue;
    }
    keys.push_back(llvm::ConstantStruct::get(
        codegen_.fieldDescTy_, {codegen_.stringLowerer().utf8Constant(key), codegen_.builder_.getInt32(uint32_t(key.size())),
                       codegen_.builder_.getInt32(uint32_t(offsets->getElementOffset(unsigned(6 + i)))),
                       codegen_.typeDescriptors().typeDescriptor(statics[i]->var->type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  if (!keys.empty()) {
    auto *arrayType = llvm::ArrayType::get(codegen_.fieldDescTy_, keys.size());
    auto *global = new llvm::GlobalVariable(
        *codegen_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, keys), "tz.classfields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // Error and its kinds print as the functions they are in JavaScript; so
  // does a class whose base is a call, `class A extends Mixin(B)`, as node
  // tells a class by its text and gives up at a parenthesis in its head.
  uint32_t flags = kLayoutClass | (cls->builtin || hasCall(cls->baseExpr) ? kLayoutBuiltin : 0);
  // Its number tells it from the other objects of a union (see plainTypeId).
  llvm::Constant *layoutInit = llvm::ConstantStruct::get(
      codegen_.layoutDescTy_,
      {drop, fields, codegen_.builder_.getInt32(uint32_t(keys.size())), codegen_.builder_.getInt32(flags),
       codegen_.stringLowerer().utf8Constant(cls->name), codegen_.builder_.getInt32(uint32_t(cls->name.size())),
       codegen_.builder_.getInt32(codegen_.unionLowerer().plainTypeId(Type::classRef(cls, cls->name)))});
  auto *layout = new llvm::GlobalVariable(*codegen_.module_, codegen_.layoutDescTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, layoutInit,
                                          "tz.classlayout." + name);
  layout->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  // Counted as any object is, so not a constant.
  llvm::Constant *info = (cls->reflectNew || cls->reflectMethods) && !cls->builtin
                             ? codegen_.classInfoLowerer().classInfoGlobal(cls)
                             : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  std::vector<llvm::Constant *> init{codegen_.builder_.getInt64(kStaticRefCount),
                                     layout,
                                     parent,
                                     codegen_.builder_.getInt32(cls->id),
                                     codegen_.builder_.getInt32(std::max(cls->id, cls->lastId)),
                                     info};
  for (size_t i = 6; i < elements.size(); ++i) {
    init.push_back(llvm::Constant::getNullValue(elements[i]));
  }
  auto *object = new llvm::GlobalVariable(
      *codegen_.module_, type, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
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
      llvm::ArrayRef<llvm::Constant *>{codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(element)});
}

// The elements as String() makes them, an undefined one empty, and the
// separator between them.
llvm::Value *UnknownLowerer::emitTupleJoin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *tuple = member->object->type;
  bool hasSeparator = !call->args.empty();
  CodeGen::Operand object = codegen_.refCounts().emitOperand(member->object, !hasSeparator || AssignLowerer::cannotAssign(call->args[0]));
  codegen_.throwLowerer().holdOperand(object, hasSeparator && codegen_.mayThrow().exprThrows(call->args[0]));
  CodeGen::Operand separator = hasSeparator ? codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true)
                                   : CodeGen::Operand{codegen_.stringLowerer().stringLiteral(u","), false, Type::string()};
  codegen_.throwLowerer().takeOperand(object);
  std::vector<CodeGen::Operand> parts;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    if (i > 0) {
      parts.push_back({separator.value, false, Type::string()});
    }
    const Type *type = tuple->fields()[i].type;
    llvm::Value *element = codegen_.builder_.CreateLoad(
        codegen_.moduleBuilder().lower(type), codegen_.objectLayouts().fieldAddress(object.value, tuple, std::to_string(i)), "element");
    codegen_.refCounts().retainValue(element, type);
    llvm::Value *text;
    if (type->isUndefined()) {
      text = codegen_.stringLowerer().stringLiteral(u"");
    } else if (type->isOptional()) {
      llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("join.value");
      llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("join.end");
      llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
      codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(element, type), valueBlock, endBlock);
      codegen_.functionFrame().switchTo(valueBlock);
      llvm::Value *shown =
          codegen_.exprLowerer().emitConversion(codegen_.optionalLowerer().optionalValue(element, type), type->inner(), Type::string());
      llvm::BasicBlock *valueEnd = codegen_.builder_.GetInsertBlock();
      codegen_.builder_.CreateBr(endBlock);
      codegen_.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.strTy_, 2);
      phi->addIncoming(codegen_.stringLowerer().stringLiteral(u""), entry);
      phi->addIncoming(shown, valueEnd);
      text = phi;
    } else {
      text = codegen_.exprLowerer().emitConversion(element, type, Type::string());
    }
    parts.push_back({text, true, Type::string()});
  }
  llvm::Value *result = parts.empty() ? codegen_.stringLowerer().stringLiteral(u"") : codegen_.stringLowerer().concatOperands(parts);
  codegen_.refCounts().dropOperand(separator);
  codegen_.refCounts().dropOperand(object);

  return result;
}

} // namespace tz
