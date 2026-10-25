// unknown, object and classes as values (docs/decorators.md, section 4).
//
// An unknown is a pointer, null for undefined. An object of the language (an
// instance, a literal, a date, a dictionary, an iterator, a class) goes in as
// the reference it is; any other value goes into a box of the runtime, an
// object whose layout says what it holds (TzBox in runtime/core/tzrt/tzrt.h).
// An unknown is counted and released as an object is, through the drop function
// of its layout. A value a check showed comes out of the box by the runtime's
// functions, or is the reference itself.
//
// A class as a value is a constant object, TzClass, made the first time the
// code uses one: its layout names it and has TZ_LAYOUT_CLASS, and it points
// to the class it extends and to its description for reflection, if
// reflection reaches it (see codegen_reflect.cpp). Its count is huge, so that
// counting it, as the code does for any object, never frees it.

#include "compiler/lower/codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

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
  if (!expr)
    return false;
  if (isa<CallExpr>(expr))
    return true;
  if (auto *member = dyn_cast<MemberExpr>(expr))
    return hasCall(member->object);
  if (auto *index = dyn_cast<IndexExpr>(expr))
    return hasCall(index->object) || hasCall(index->index);
  if (auto *conv = dyn_cast<CastExpr>(expr))
    return hasCall(conv->operand);
  return false;
}

// A reference to an object of the language, which an unknown holds as it is.
bool isObjectHeaded(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

llvm::FunctionCallee CodeGen::unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                              llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::boxValue(llvm::Value *value, const Type *type) {
  llvm::Type *ptr = builder_.getPtrTy();
  // Reflect.apply may call it (see finishReflection).
  if (type->isFunction())
    unknownFunctionTypes_.insert(type);
  // A value that may hold a member of a group goes into a box that is one
  // (runtime/core/unknown/unknown.c).
  const char *box = mayHoldMember(type) ? "tz_unknown_box_member" : "tz_unknown_box";
  return builder_.CreateCall(unknownFunction(box, ptr, {ptr, ptr}),
                             {typeDescriptor(type), spill(value, "box.value")}, "box");
}

llvm::Value *CodeGen::emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = builder_.getPtrTy();
  if (to->isUnknown()) {
    if (from->isUnknown() || isObjectHeaded(from))
      return value;
    if (from->isUndefined())
      return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    // A union or a literal type: what it holds, as tz_unknown_of finds it
    // (a reference as it is, any other value in a box of its own type; a
    // string literal as its string).
    if (from->nonOptional()->isUnion() || from->nonOptional()->isLiteral())
      return builder_.CreateCall(unknownFunction("tz_unknown_of", ptr, {ptr, ptr}),
                                 {typeDescriptor(from), spill(value, "union.value")}, "unknown");
    if (!from->isOptional())
      return boxValue(value, from);
    const Type *inner = from->inner();
    if (isObjectHeaded(inner)) // null for undefined
      return value;
    // A value in a box, or null for undefined.
    llvm::BasicBlock *boxBlock = newBlock("unknown.box");
    llvm::BasicBlock *endBlock = newBlock("unknown.end");
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    builder_.CreateCondBr(optionalHas(value, from), boxBlock, endBlock);
    switchTo(boxBlock);
    llvm::Value *box = emitUnknownConversion(optionalValue(value, from), inner, to);
    llvm::BasicBlock *boxEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(ptr, 2);
    phi->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()), entry);
    phi->addIncoming(box, boxEnd);
    return phi;
  }
  // What a check showed: the value in the box, or the reference itself.
  llvm::Value *result;
  if (to->isString()) {
    result = callReturningString(
        unknownFunction("tz_unknown_string", builder_.getVoidTy(), {ptr, ptr}), {value});
  } else if (to->isFloat()) {
    result = builder_.CreateCall(unknownFunction("tz_unknown_number", builder_.getDoubleTy(), {ptr}),
                                 {value});
  } else if (to->isBool()) {
    llvm::Value *flag =
        builder_.CreateCall(unknownFunction("tz_unknown_bool", builder_.getInt32Ty(), {ptr}), {value});
    result = builder_.CreateICmpNE(flag, builder_.getInt32(0));
  } else if (to->isArray()) {
    result = builder_.CreateCall(unknownFunction("tz_unknown_array", ptr, {ptr, ptr}),
                                 {value, typeDescriptor(to)});
  } else {
    return value; // an object, of a class or any
  }
  releaseValue(value, from);
  return result;
}

llvm::Value *CodeGen::emitUnknownTypeOf(Expr *operand) {
  Operand value = emitOperand(operand, /*canBorrow=*/true);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *kind = builder_.CreateCall(
      unknownFunction("tz_unknown_kind", builder_.getInt32Ty(), {ptr}), {value.value}, "kind");
  dropOperand(value);
  llvm::Value *text = stringLiteral(kKindNames[0]);
  for (unsigned k = 1; k < std::size(kKindNames); ++k)
    text = builder_.CreateSelect(builder_.CreateICmpEQ(kind, builder_.getInt32(k)),
                                 stringLiteral(kKindNames[k]), text);
  return text;
}

llvm::Value *CodeGen::emitUnknownIsArray(Expr *arg) {
  Operand value = emitOperand(arg, /*canBorrow=*/true);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *array = builder_.CreateCall(
      unknownFunction("tz_unknown_is_array", builder_.getInt32Ty(), {ptr}), {value.value});
  dropOperand(value);
  return builder_.CreateICmpNE(array, builder_.getInt32(0));
}

// { TzClass, the static fields }: the static fields of a class used as a
// value live in its object, whose layout describes those that are keys, so
// that it prints as node prints a class, `[class User] { count: 0 }`.
llvm::Constant *CodeGen::classObject(const ClassDecl *cls) {
  if (llvm::Constant *found = classObjects_.lookup(cls))
    return found;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  std::vector<const ClassField *> statics;
  std::vector<llvm::Type *> elements{i64, ptr, ptr, i32, i32, ptr};
  if (cls->usedAsValue)
    for (const ClassField &f : cls->fields)
      if (f.isStatic && f.var) {
        statics.push_back(&f);
        elements.push_back(lower(f.var->type));
      }
  std::string name = symbolPrefixes_.lookup(cls) + cls->name.str();
  auto *type = llvm::StructType::create(ctx_, elements, "tz.class." + name);
  classObjectTypes_[cls] = type;
  llvm::Constant *parent = cls->parent ? classObject(cls->parent)
                                       : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  auto *drop = cast<llvm::Function>(
      unknownFunction("tz_static_drop", builder_.getVoidTy(), {ptr}).getCallee());
  // The keys: the static fields but the `#private` ones, as node shows them.
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(type);
  std::vector<llvm::Constant *> keys;
  for (size_t i = 0; i < statics.size(); ++i) {
    llvm::StringRef key = statics[i]->name;
    if (key.starts_with("#"))
      continue;
    keys.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(key), builder_.getInt32(uint32_t(key.size())),
                       builder_.getInt32(uint32_t(offsets->getElementOffset(unsigned(6 + i)))),
                       typeDescriptor(statics[i]->var->type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!keys.empty()) {
    auto *arrayType = llvm::ArrayType::get(fieldDescTy_, keys.size());
    auto *global = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arrayType, keys),
                                            "tz.classfields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // Error and its kinds print as the functions they are in JavaScript; so
  // does a class whose base is a call, `class A extends Mixin(B)`, as node
  // tells a class by its text and gives up at a parenthesis in its head.
  uint32_t flags = kLayoutClass | (cls->builtin || hasCall(cls->baseExpr) ? kLayoutBuiltin : 0);
  // Its number tells it from the other objects of a union (see plainTypeId).
  llvm::Constant *layoutInit = llvm::ConstantStruct::get(
      layoutDescTy_, {drop, fields, builder_.getInt32(uint32_t(keys.size())),
                      builder_.getInt32(flags), utf8Constant(cls->name),
                      builder_.getInt32(uint32_t(cls->name.size())),
                      builder_.getInt32(plainTypeId(Type::classRef(cls, cls->name)))});
  auto *layout = new llvm::GlobalVariable(*module_, layoutDescTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, layoutInit,
                                          "tz.classlayout." + name);
  layout->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  // Counted as any object is, so not a constant.
  llvm::Constant *info = (cls->reflectNew || cls->reflectMethods) && !cls->builtin
                             ? classInfoGlobal(cls)
                             : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  std::vector<llvm::Constant *> init{builder_.getInt64(kStaticRefCount), layout, parent,
                                     builder_.getInt32(cls->id),
                                     builder_.getInt32(std::max(cls->id, cls->lastId)), info};
  for (size_t i = 6; i < elements.size(); ++i)
    init.push_back(llvm::Constant::getNullValue(elements[i]));
  auto *object = new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                          llvm::GlobalValue::InternalLinkage,
                                          llvm::ConstantStruct::get(type, init), "tz.class." + name);
  classObjects_[cls] = object;
  return object;
}

llvm::Constant *CodeGen::staticFieldAddress(const ClassDecl *cls, const VarSymbol *var) {
  llvm::Constant *object = classObject(cls);
  unsigned element = 6;
  for (const ClassField &f : cls->fields)
    if (f.isStatic && f.var) {
      if (f.var == var)
        break;
      ++element;
    }
  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      classObjectTypes_.lookup(cls), object,
      llvm::ArrayRef<llvm::Constant *>{builder_.getInt32(0), builder_.getInt32(element)});
}

// The elements as String() makes them, an undefined one empty, and the
// separator between them.
llvm::Value *CodeGen::emitTupleJoin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *tuple = member->object->type;
  bool hasSeparator = !call->args.empty();
  Operand object =
      emitOperand(member->object, !hasSeparator || cannotAssign(call->args[0]));
  holdOperand(object, hasSeparator && exprThrows(call->args[0]));
  Operand separator = hasSeparator ? emitOperand(call->args[0], /*canBorrow=*/true)
                                   : Operand{stringLiteral(u","), false, Type::string()};
  takeOperand(object);
  std::vector<Operand> parts;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    if (i > 0)
      parts.push_back({separator.value, false, Type::string()});
    const Type *type = tuple->fields()[i].type;
    llvm::Value *element = builder_.CreateLoad(
        lower(type), fieldAddress(object.value, tuple, std::to_string(i)), "element");
    retainValue(element, type);
    llvm::Value *text;
    if (type->isUndefined()) {
      text = stringLiteral(u"");
    } else if (type->isOptional()) {
      llvm::BasicBlock *valueBlock = newBlock("join.value");
      llvm::BasicBlock *endBlock = newBlock("join.end");
      llvm::BasicBlock *entry = builder_.GetInsertBlock();
      builder_.CreateCondBr(optionalHas(element, type), valueBlock, endBlock);
      switchTo(valueBlock);
      llvm::Value *shown = emitConversion(optionalValue(element, type), type->inner(), Type::string());
      llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
      builder_.CreateBr(endBlock);
      switchTo(endBlock);
      llvm::PHINode *phi = builder_.CreatePHI(strTy_, 2);
      phi->addIncoming(stringLiteral(u""), entry);
      phi->addIncoming(shown, valueEnd);
      text = phi;
    } else {
      text = emitConversion(element, type, Type::string());
    }
    parts.push_back({text, true, Type::string()});
  }
  llvm::Value *result = parts.empty() ? stringLiteral(u"") : concatOperands(parts);
  dropOperand(separator);
  dropOperand(object);
  return result;
}

} // namespace tz
