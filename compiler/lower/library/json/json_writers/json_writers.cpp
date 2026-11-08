#include "compiler/lower/library/json/json_writers/json_writers.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"

namespace tz {

JsonWriters::JsonWriters(Lowering &owner)
    : lowering(owner) {}

bool JsonWriters::skipped(const Type *type) {
  const Type *t = type->nonOptional();

  return t->isFunction() || t->isUndefined() || t->isVoid() || t->isNever();
}

bool JsonWriters::plainName(llvm::StringRef name) {
  for (char c : name) {
    auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte >= 0x80 || byte == '"' || byte == '\\') {
      return false;
    }
  }

  return true;
}

bool JsonWriters::plainObject(const Type *type) const {
  return type->isObject() && !type->isClass() && !type->isTuple() && !type->isAnyObject() &&
         !type->isIncomplete() && !type->isRegExp() && !type->isMatch() && !type->isDate() &&
         !type->isIntl() && !type->isWeakRef() && !type->isTimer() && !type->isCrypto() &&
         !type->isBytes() && !type->isRuntimeObject() && !this->lowering.isCyclic(type);
}

bool JsonWriters::writable(const Type *type) {
  llvm::SmallPtrSet<const Type *, 8> open;

  return this->check(type, open);
}

bool JsonWriters::check(const Type *type, llvm::SmallPtrSetImpl<const Type *> &open) {
  if (auto found = this->writableTypes.find(type); found != this->writableTypes.end()) {
    return found->second;
  }
  bool result = false;
  switch (type->kind()) {
  case Type::Kind::String:
  case Type::Kind::Int:
  case Type::Kind::Double:
  case Type::Kind::Bool:
    result = true;
    break;
  case Type::Kind::Optional:
    result = !type->inner()->isOptional() && !JsonWriters::skipped(type->inner()) &&
             this->check(type->inner(), open);
    break;
  case Type::Kind::Array:
    result = !JsonWriters::skipped(type->inner()) && this->check(type->inner(), open);
    break;
  case Type::Kind::Object:
    if (!this->plainObject(type) || !open.insert(type).second) {
      return false;
    }
    result = true;
    for (const Field &field : type->fields()) {
      if (!JsonWriters::plainName(field.name) ||
          (!JsonWriters::skipped(field.type) && !this->check(field.type, open))) {
        result = false;
        break;
      }
    }
    open.erase(type);
    break;
  default:
    break;
  }
  this->writableTypes[type] = result;

  return result;
}

llvm::FunctionCallee JsonWriters::runtime(llvm::StringRef name, llvm::Type *ret,
                                          llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Function *JsonWriters::writer(const Type *type, bool array) {
  llvm::Function *&fn = array ? this->arrays[type] : this->objects[type];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *fnType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {ptr, ptr}, false);
  fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                              array ? "tz.json.array" : "tz.json.object",
                              this->lowering.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  this->pending.push_back({type, fn, array});

  return fn;
}

void JsonWriters::writeRaw(llvm::Value *out, llvm::Value *text, llvm::Value *length) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  this->lowering.builder.CreateCall(this->runtime("tz_json_out_raw",
                                                  this->lowering.builder.getVoidTy(),
                                                  {ptr, ptr, this->lowering.builder.getInt64Ty()}),
                                    {out, text, length});
}

void JsonWriters::writeText(llvm::Value *out, llvm::StringRef text) {
  this->writeRaw(out, this->lowering.stringLowerer().utf8Constant(text),
                 this->lowering.builder.getInt64(text.size()));
}

void JsonWriters::writeValue(llvm::Value *out, const Type *type, llvm::Value *value) {
  llvm::IRBuilder<> &b = this->lowering.builder;
  llvm::Type *ptr = b.getPtrTy();
  llvm::Type *i64 = b.getInt64Ty();
  switch (type->kind()) {
  case Type::Kind::String:
    b.CreateCall(this->runtime("tz_json_out_str", b.getVoidTy(), {ptr, ptr, i64}),
                 {out, this->lowering.stringLowerer().stringObj(value),
                  this->lowering.stringLowerer().stringMeta(value)});
    return;
  case Type::Kind::Int: {
    bool wide = type->bitWidth() == 64 && type->isUnsigned();
    llvm::Value *number = value;
    if (type->bitWidth() < 64) {
      number = type->isUnsigned() ? b.CreateZExt(value, i64) : b.CreateSExt(value, i64);
    }
    b.CreateCall(
        this->runtime(wide ? "tz_json_out_uint" : "tz_json_out_int", b.getVoidTy(), {ptr, i64}),
        {out, number});
    return;
  }
  case Type::Kind::Double:
    b.CreateCall(this->runtime("tz_json_out_double", b.getVoidTy(), {ptr, b.getDoubleTy()}),
                 {out, value});
    return;
  case Type::Kind::Bool:
    this->writeRaw(out,
                   b.CreateSelect(value, this->lowering.stringLowerer().utf8Constant("true"),
                                  this->lowering.stringLowerer().utf8Constant("false")),
                   b.CreateSelect(value, b.getInt64(4), b.getInt64(5)));
    return;
  case Type::Kind::Array:
    b.CreateCall(this->writer(type->inner(), true), {out, value});
    return;
  case Type::Kind::Object:
    b.CreateCall(this->writer(type, false), {out, value});
    return;
  case Type::Kind::Optional: {
    llvm::Function *fn = b.GetInsertBlock()->getParent();
    auto *someBlock = llvm::BasicBlock::Create(this->lowering.ctx, "json.some", fn);
    auto *noneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "json.none", fn);
    auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "json.done", fn);
    b.CreateCondBr(this->lowering.optionalLowerer().optionalHas(value, type), someBlock, noneBlock);
    b.SetInsertPoint(someBlock);
    this->writeValue(out, type->inner(),
                     this->lowering.optionalLowerer().optionalValue(value, type));
    b.CreateBr(doneBlock);
    b.SetInsertPoint(noneBlock);
    this->writeText(out, "null");
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    return;
  }
  default:
    llvm_unreachable("JsonWriters::writable lets only these types through");
  }
}

llvm::Value *JsonWriters::emitStringify(llvm::Value *value, const Type *type) {
  llvm::IRBuilder<> &b = this->lowering.builder;
  llvm::Type *ptr = b.getPtrTy();
  llvm::Value *out = b.CreateCall(this->runtime("tz_json_out_begin", ptr, {}), {}, "json.out");
  this->writeValue(out, type, value);
  llvm::Value *slot =
      this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "json.text");
  b.CreateCall(this->runtime("tz_json_out_end", b.getVoidTy(), {ptr, ptr}), {out, slot});

  return b.CreateLoad(this->lowering.strTy, slot, "json.text");
}

void JsonWriters::emitArrayBody(const Type *element, llvm::Function *fn) {
  llvm::IRBuilder<> &b = this->lowering.builder;
  auto *entry = llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn);
  auto *loopBlock = llvm::BasicBlock::Create(this->lowering.ctx, "loop", fn);
  auto *commaBlock = llvm::BasicBlock::Create(this->lowering.ctx, "comma", fn);
  auto *bodyBlock = llvm::BasicBlock::Create(this->lowering.ctx, "element", fn);
  auto *endBlock = llvm::BasicBlock::Create(this->lowering.ctx, "end", fn);
  llvm::Value *out = fn->getArg(0);
  llvm::Value *array = fn->getArg(1);
  b.SetInsertPoint(entry);
  this->writeText(out, "[");
  llvm::Value *length = this->lowering.arrayLowerer().arrayLength(array);
  llvm::Value *data = this->lowering.arrayLowerer().arrayData(array);
  b.CreateBr(loopBlock);
  b.SetInsertPoint(loopBlock);
  llvm::PHINode *index = b.CreatePHI(b.getInt64Ty(), 2, "index");
  index->addIncoming(b.getInt64(0), entry);
  auto *testBlock = llvm::BasicBlock::Create(this->lowering.ctx, "test", fn);
  b.CreateCondBr(b.CreateICmpULT(index, length), testBlock, endBlock);
  b.SetInsertPoint(testBlock);
  b.CreateCondBr(b.CreateICmpEQ(index, b.getInt64(0)), bodyBlock, commaBlock);
  b.SetInsertPoint(commaBlock);
  this->writeText(out, ",");
  b.CreateBr(bodyBlock);
  b.SetInsertPoint(bodyBlock);
  llvm::Value *address = this->lowering.arrayLowerer().elementAddress(data, element, index);
  llvm::Value *value =
      b.CreateLoad(this->lowering.moduleBuilder().lower(element), address, "element");
  this->writeValue(out, element, value);
  index->addIncoming(b.CreateAdd(index, b.getInt64(1)), b.GetInsertBlock());
  b.CreateBr(loopBlock);
  b.SetInsertPoint(endBlock);
  this->writeText(out, "]");
  b.CreateRetVoid();
}

void JsonWriters::emitFields(const Type *type, llvm::ArrayRef<unsigned> keys, llvm::Value *out,
                             llvm::Value *object, llvm::Value *written) {
  llvm::IRBuilder<> &b = this->lowering.builder;
  llvm::Function *fn = b.GetInsertBlock()->getParent();
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  enum { None, Some, Maybe } state = None;
  b.CreateStore(b.getFalse(), written);
  this->writeText(out, "{");
  auto writeKey = [&](const Field &field) {
    std::string text = ",\"" + field.name + "\":";
    llvm::Constant *key = this->lowering.stringLowerer().utf8Constant(text);
    llvm::Value *size = b.getInt64(text.size());
    if (state == Some) {
      this->writeRaw(out, key, size);
      return;
    }
    llvm::Value *rest = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), key, 1);
    if (state == None) {
      this->writeRaw(out, rest, b.getInt64(text.size() - 1));
      return;
    }
    llvm::Value *before = b.CreateLoad(b.getInt1Ty(), written, "written");
    this->writeRaw(out, b.CreateSelect(before, key, rest),
                   b.CreateSelect(before, size, b.getInt64(text.size() - 1)));
  };
  for (unsigned k : keys) {
    const Field &field = type->fields()[k];
    if (JsonWriters::skipped(field.type)) {
      continue;
    }
    llvm::Value *address = b.CreateStructGEP(layout.type, object, layout.elements[k]);
    llvm::Value *value =
        b.CreateLoad(this->lowering.moduleBuilder().lower(field.type), address, field.name);
    if (!field.type->isOptional()) {
      writeKey(field);
      this->writeValue(out, field.type, value);
      state = Some;
      continue;
    }
    auto *someBlock = llvm::BasicBlock::Create(this->lowering.ctx, "field.some", fn);
    auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "field.done", fn);
    b.CreateCondBr(this->lowering.optionalLowerer().optionalHas(value, field.type), someBlock,
                   doneBlock);
    b.SetInsertPoint(someBlock);
    writeKey(field);
    this->writeValue(out, field.type->inner(),
                     this->lowering.optionalLowerer().optionalValue(value, field.type));
    b.CreateStore(b.getTrue(), written);
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    if (state == None) {
      state = Maybe;
    }
  }
  this->writeText(out, "}");
}

void JsonWriters::emitObjectBody(const Type *type, llvm::Function *fn) {
  llvm::IRBuilder<> &b = this->lowering.builder;
  auto *entry = llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn);
  llvm::Value *out = fn->getArg(0);
  llvm::Value *object = fn->getArg(1);
  b.SetInsertPoint(entry);
  llvm::Value *slot = b.CreateAlloca(b.getPtrTy(), nullptr, "object.slot");
  llvm::Value *written = b.CreateAlloca(b.getInt1Ty(), nullptr, "written");
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  llvm::Value *descriptor =
      b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(layout.type, object, 1), "layout");
  auto &descriptors = this->lowering.layoutDescriptors;
  for (auto it = descriptors.lower_bound({type, {}}); it != descriptors.end() && it->first.first == type;
       ++it) {
    const std::vector<unsigned> &key = it->first.second;
    if (key.empty() || key.back() != ~0u) {
      continue;
    }
    auto *matchBlock = llvm::BasicBlock::Create(this->lowering.ctx, "layout", fn);
    auto *nextBlock = llvm::BasicBlock::Create(this->lowering.ctx, "next", fn);
    b.CreateCondBr(b.CreateICmpEQ(descriptor, it->second), matchBlock, nextBlock);
    b.SetInsertPoint(matchBlock);
    this->emitFields(type, llvm::ArrayRef<unsigned>(key).drop_back(), out, object, written);
    b.CreateRetVoid();
    b.SetInsertPoint(nextBlock);
  }
  b.CreateStore(object, slot);
  b.CreateCall(this->runtime("tz_json_out_value", b.getInt32Ty(),
                             {b.getPtrTy(), b.getPtrTy(), b.getPtrTy()}),
               {out, this->lowering.typeDescriptors().typeDescriptor(type), slot});
  b.CreateRetVoid();
}

void JsonWriters::finish() {
  if (this->pending.empty()) {
    return;
  }
  llvm::IRBuilder<> &b = this->lowering.builder;
  llvm::IRBuilderBase::InsertPoint point = b.saveIP();
  llvm::DebugLoc loc = b.getCurrentDebugLocation();
  b.SetCurrentDebugLocation(llvm::DebugLoc());
  while (!this->pending.empty()) {
    Pending next = this->pending.back();
    this->pending.pop_back();
    if (next.array) {
      this->emitArrayBody(next.type, next.fn);
    } else {
      this->emitObjectBody(next.type, next.fn);
    }
  }
  b.restoreIP(point);
  b.SetCurrentDebugLocation(loc);
}

}
