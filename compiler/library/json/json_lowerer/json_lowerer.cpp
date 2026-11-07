#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/json/json_writers/json_writers.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int32_t kJsonArray = 4;

}

std::u16string JsonLowerer::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

bool JsonLowerer::isNullLiteral(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && !name->var() && !name->func() && name->type && name->type->isJson();
}

bool JsonLowerer::isJsonish(const Type *type) {
  return type->nonOptional()->isJson();
}

JsonLowerer::JsonLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee JsonLowerer::jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                               llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *JsonLowerer::jsonPresent(llvm::Value *value) {
  llvm::Value *nullish = this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_is_nullish", this->lowering.builder_.getInt32Ty(),
                         {this->lowering.builder_.getPtrTy()}),
      {value});

  return this->lowering.builder_.CreateICmpEQ(nullish, this->lowering.builder_.getInt32(0));
}

llvm::Value *JsonLowerer::emitJsonNull() {
  return this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_null", this->lowering.builder_.getPtrTy(), {}), {});
}

llvm::Value *JsonLowerer::emitJsonMember(MemberExpr *member) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
  llvm::Value *key = this->lowering.stringLowerer().stringLiteral(JsonLowerer::utf16(member->name));
  llvm::Value *result = this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_get", ptr, {ptr, ptr, this->lowering.builder_.getInt64Ty()}),
      {object.value, this->lowering.stringLowerer().stringObj(key),
       this->lowering.stringLowerer().stringMeta(key)},
      member->name);
  this->lowering.refCounts().dropOperand(object);

  return result;
}

llvm::Value *JsonLowerer::emitJsonIndex(IndexExpr *index) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  Lowering::Operand object = this->lowering.refCounts().emitOperand(
      index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *result;
  if (index->index->type->isString()) {
    Lowering::Operand key = this->lowering.refCounts().emitOperand(index->index, true);
    result = this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_get", ptr, {ptr, ptr, this->lowering.builder_.getInt64Ty()}),
        {object.value, this->lowering.stringLowerer().stringObj(key.value),
         this->lowering.stringLowerer().stringMeta(key.value)});
    this->lowering.refCounts().dropOperand(key);
  } else {
    llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
    result = this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_get_index", ptr, {ptr, this->lowering.builder_.getDoubleTy()}),
        {object.value, position});
  }
  this->lowering.refCounts().dropOperand(object);

  return result;
}

llvm::Value *JsonLowerer::emitJsonAssign(AssignExpr *assign, bool needResult) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *objectExpr = index ? index->object : member->object;
  bool later = AssignLowerer::cannotAssign(assign->value) &&
               (!index || AssignLowerer::cannotAssign(index->index));
  Lowering::Operand object = this->lowering.refCounts().emitOperand(objectExpr, later);
  Lowering::Operand key{nullptr, false, Type::string()};
  llvm::Value *position = nullptr;
  if (member) {
    key.value = this->lowering.stringLowerer().stringLiteral(JsonLowerer::utf16(member->name));
  } else if (index->index->type->isString()) {
    key = this->lowering.refCounts().emitOperand(index->index,
                                                 AssignLowerer::cannotAssign(assign->value));
  } else {
    position = this->lowering.exprLowerer().emitExpr(index->index);
  }
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(assign->value);
  if (needResult) {
    this->lowering.refCounts().retainValue(value, assign->type);
  }
  if (position) {
    this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_set_index", this->lowering.builder_.getVoidTy(),
                           {ptr, this->lowering.builder_.getDoubleTy(), ptr}),
        {object.value, position, value});
  } else {
    this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_set", this->lowering.builder_.getVoidTy(),
                           {ptr, ptr, i64, ptr}),
        {object.value, this->lowering.stringLowerer().stringObj(key.value),
         this->lowering.stringLowerer().stringMeta(key.value), value});
  }
  this->lowering.refCounts().dropOperand(key);
  this->lowering.refCounts().dropOperand(object);

  return needResult ? value : nullptr;
}

llvm::Value *JsonLowerer::emitJsonConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *f64 = this->lowering.builder_.getDoubleTy();
  if (JsonLowerer::isJsonish(to)) {
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
    }
    if (from->isRecord()) {
      return this->emitJsonFromRecord(value, from);
    }
    if (from->isOptional()) {
      llvm::BasicBlock *before = this->lowering.builder_.GetInsertBlock();
      llvm::BasicBlock *convertBlock = this->lowering.functionFrame().newBlock("json.convert");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("json.converted");
      this->lowering.builder_.CreateCondBr(
          this->lowering.optionalLowerer().optionalHas(value, from), convertBlock, endBlock);
      this->lowering.functionFrame().switchTo(convertBlock);
      llvm::Value *converted = this->emitJsonConversion(
          this->lowering.optionalLowerer().optionalValue(value, from), from->inner(), Type::json());
      llvm::BasicBlock *convertEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = this->lowering.builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), before);
      phi->addIncoming(converted, convertEnd);
      return phi;
    }
    if (from->isJson()) {
      return value;
    }
    if (from->isBool()) {
      return this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_bool", ptr, {i32}),
                                                {this->lowering.builder_.CreateZExt(value, i32)});
    }
    if (from->isInteger()) {
      value = this->lowering.builder_.CreateSIToFP(value, f64);
    }
    if (from->isNumeric()) {
      return this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_number", ptr, {f64}),
                                                {value});
    }
    if (from->isString()) {
      llvm::Value *node = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_string", ptr, {ptr, this->lowering.builder_.getInt64Ty()}),
          {this->lowering.stringLowerer().stringObj(value),
           this->lowering.stringLowerer().stringMeta(value)});
      this->lowering.refCounts().releaseString(value);
      return node;
    }
    return this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_from_array", ptr, {ptr}),
                                              {value});
  }

  if (to->isOptional()) {
    return this->lowering.optionalLowerer().makeOptional(
        this->emitJsonConversion(value, from, to->inner()), this->lowering.builder_.getTrue(), to);
  }
  llvm::Value *result;
  if (to->isString()) {
    result = this->lowering.stringLowerer().callReturningString(
        this->jsonFunction("tz_json_as_string", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {value});
  } else if (to->isFloat()) {
    result = this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_as_number", f64, {ptr}),
                                                {value});
  } else if (to->isBool()) {
    result = this->lowering.builder_.CreateTrunc(
        this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_as_bool", i32, {ptr}),
                                           {value}),
        this->lowering.builder_.getInt1Ty());
  } else {
    result = this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_as_array", ptr, {ptr}),
                                                {value});
  }
  this->lowering.refCounts().releaseValue(value, from);

  return result;
}

llvm::Value *JsonLowerer::emitJsonEquality(BinaryExpr *bin) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs;
  Expr *rhs = bin->rhs;
  bool lhsNothing = lhs->type->isUndefined() || JsonLowerer::isNullLiteral(lhs);
  bool rhsNothing = rhs->type->isUndefined() || JsonLowerer::isNullLiteral(rhs);
  if (lhsNothing || rhsNothing) {
    if (lhsNothing && rhsNothing) {
      bool same = JsonLowerer::isNullLiteral(lhs) == JsonLowerer::isNullLiteral(rhs);
      this->lowering.stmtLowerer().emitDiscarded(lhs);
      this->lowering.stmtLowerer().emitDiscarded(rhs);
      return this->lowering.builder_.getInt1(isEq == same);
    }
    if (lhs->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(lhs);
    }
    Expr *other = lhsNothing ? rhs : lhs;
    bool null = JsonLowerer::isNullLiteral(lhsNothing ? lhs : rhs);
    Lowering::Operand value = this->lowering.refCounts().emitOperand(other, true);
    if (rhs->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(rhs);
    }
    llvm::Value *matches =
        null ? this->lowering.builder_.CreateICmpNE(
                   this->lowering.builder_.CreateCall(
                       this->jsonFunction("tz_json_is_null", i32, {ptr}), {value.value}),
                   this->lowering.builder_.getInt32(0))
             : this->lowering.builder_.CreateIsNull(value.value);
    this->lowering.refCounts().dropOperand(value);
    return isEq ? matches : this->lowering.builder_.CreateNot(matches);
  }
  Lowering::Operand a =
      this->lowering.refCounts().emitOperand(lhs, AssignLowerer::cannotAssign(rhs));
  Lowering::Operand b = this->lowering.refCounts().emitOperand(rhs, true);
  llvm::Value *equal;
  if (JsonLowerer::isJsonish(lhs->type) && JsonLowerer::isJsonish(rhs->type)) {
    equal = this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_strict_equal", i32, {ptr, ptr}), {a.value, b.value});
  } else {
    const Lowering::Operand &json = JsonLowerer::isJsonish(lhs->type) ? a : b;
    const Lowering::Operand &other = JsonLowerer::isJsonish(lhs->type) ? b : a;
    if (other.type->isString()) {
      equal = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_equals_string", i32, {ptr, ptr, i64}),
          {json.value, this->lowering.stringLowerer().stringObj(other.value),
           this->lowering.stringLowerer().stringMeta(other.value)});
    } else if (other.type->isBool()) {
      equal = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_equals_bool", i32, {ptr, i32}),
          {json.value, this->lowering.builder_.CreateZExt(other.value, i32)});
    } else {
      equal = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_equals_number", i32,
                             {ptr, this->lowering.builder_.getDoubleTy()}),
          {json.value, other.value});
    }
  }
  this->lowering.refCounts().dropOperand(a);
  this->lowering.refCounts().dropOperand(b);
  llvm::Value *same =
      this->lowering.builder_.CreateICmpNE(equal, this->lowering.builder_.getInt32(0));

  return isEq ? same : this->lowering.builder_.CreateNot(same);
}

llvm::Value *JsonLowerer::emitJsonNullish(BinaryExpr *bin) {
  llvm::Value *lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = this->lowering.functionFrame().newBlock("nullish.default");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("nullish.end");
  this->lowering.builder_.CreateCondBr(this->jsonPresent(lhs), valueBlock, defaultBlock);

  this->lowering.functionFrame().switchTo(valueBlock);
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(defaultBlock);
  this->lowering.refCounts().releaseValue(lhs, bin->lhs->type);
  llvm::Value *fallback = this->lowering.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(lhs, valueBlock);
  phi->addIncoming(fallback, defaultEnd);

  return phi;
}

llvm::Value *JsonLowerer::emitJsonChainLink(CastExpr *link) {
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("chain.link");
  this->lowering.builder_.CreateCondBr(this->jsonPresent(value), valueBlock,
                                       this->lowering.chainEnds_.back());
  this->lowering.functionFrame().switchTo(valueBlock);

  return value;
}

llvm::Value *JsonLowerer::emitJsonTypeOf(Expr *operand) {
  Lowering::Operand value = this->lowering.refCounts().emitOperand(operand, true);
  llvm::Value *code = this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_typeof", this->lowering.builder_.getInt32Ty(),
                         {this->lowering.builder_.getPtrTy()}),
      {value.value});
  this->lowering.refCounts().dropOperand(value);

  return this->lowering.unknownLowerer().typeOfText(code);
}

llvm::Value *JsonLowerer::emitJsonFromRecord(llvm::Value *map, const Type *from) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::FunctionCallee put =
      this->jsonFunction("tz_json_object_put", this->lowering.builder_.getVoidTy(),
                         {ptr, ptr, this->lowering.builder_.getInt64Ty(), ptr});
  llvm::Value *object = this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_object_new", ptr, {}), {}, "json");
  const Type *value = from->valueType();
  llvm::Value *slot = this->lowering.functionFrame().spill(map, "record");
  this->lowering.dictLowerer().emitDictLoop(slot, from, nullptr, [&](llvm::Value *entry) {
    llvm::Value *key =
        this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(Type::string()),
                                           this->lowering.dictLowerer().entryKey(entry, from));
    llvm::Value *item =
        this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(value),
                                           this->lowering.dictLowerer().entryValue(entry, from));
    this->lowering.refCounts().retainValue(item, value);
    llvm::Value *json = this->emitJsonConversion(item, value, Type::json());
    llvm::Value *args[] = {object, this->lowering.stringLowerer().stringObj(key),
                           this->lowering.stringLowerer().stringMeta(key), json};
    if (!value->isOptional()) {
      this->lowering.builder_.CreateCall(put, args);
      return;
    }
    llvm::BasicBlock *putBlock = this->lowering.functionFrame().newBlock("json.put");
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("json.next");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(json), putBlock,
                                         nextBlock);
    this->lowering.functionFrame().switchTo(putBlock);
    this->lowering.builder_.CreateCall(put, args);
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(nextBlock);
  });
  this->lowering.refCounts().releaseValue(this->lowering.builder_.CreateLoad(ptr, slot), from);

  return object;
}

llvm::Value *JsonLowerer::emitJsonObjectLiteral(ObjectLitExpr *lit) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::FunctionCallee put =
      this->jsonFunction("tz_json_object_put", this->lowering.builder_.getVoidTy(),
                         {ptr, ptr, this->lowering.builder_.getInt64Ty(), ptr});
  llvm::Value *object = this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_object_new", ptr, {}), {}, "json");
  for (const ObjectLitExpr::Property &prop : lit->props) {
    Lowering::Operand key{
        this->lowering.stringLowerer().stringLiteral(JsonLowerer::utf16(prop.name)), false,
        Type::string()};
    if (prop.key()) {
      key = this->lowering.refCounts().emitOperand(prop.key(),
                                                   AssignLowerer::cannotAssign(prop.value));
    }
    llvm::Value *value = this->lowering.exprLowerer().emitExpr(prop.value);
    llvm::Value *args[] = {object, this->lowering.stringLowerer().stringObj(key.value),
                           this->lowering.stringLowerer().stringMeta(key.value), value};
    if (!prop.value->type->isOptional()) {
      this->lowering.builder_.CreateCall(put, args);
    } else {
      llvm::BasicBlock *putBlock = this->lowering.functionFrame().newBlock("json.put");
      llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("json.next");
      this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(value), putBlock,
                                           nextBlock);
      this->lowering.functionFrame().switchTo(putBlock);
      this->lowering.builder_.CreateCall(put, args);
      this->lowering.builder_.CreateBr(nextBlock);
      this->lowering.functionFrame().switchTo(nextBlock);
    }
    this->lowering.refCounts().dropOperand(key);
  }

  return object;
}

llvm::Value *JsonLowerer::emitJsonObjectCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  Lowering::Operand object = this->lowering.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin() == Builtin::ObjectKeys) {
    result = this->lowering.builder_.CreateCall(
        this->jsonFunction(call->forIn ? "tz_json_for_in_keys" : "tz_json_keys", ptr, {ptr, ptr}),
        {object.value, this->lowering.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin() == Builtin::ObjectValues) {
    result = this->lowering.builder_.CreateCall(this->jsonFunction("tz_json_values", ptr, {ptr}),
                                                {object.value}, "values");
  } else {
    uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
        this->lowering.objectLayouts().objectLayout(element).type);
    result = this->lowering.builder_.CreateCall(
        this->jsonFunction("tz_json_entries", ptr,
                           {ptr, ptr, ptr, this->lowering.builder_.getInt64Ty()}),
        {object.value, this->lowering.typeDescriptors().typeDescriptor(element),
         this->lowering.objectLayouts().layoutDescriptor(element, {0, 1}),
         this->lowering.builder_.getInt64(size)},
        "entries");
  }
  this->lowering.refCounts().dropOperand(object);

  return result;
}

void JsonLowerer::useErrors(const Type *errorType) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message"}}};
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->jsonFunction("tz_json_errors", this->lowering.builder_.getVoidTy(), {ptr}),
      {this->lowering.objectMaker().maker(errorType, orders)});
}

llvm::Value *JsonLowerer::emitJsonCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Value *noPath = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  switch (call->builtin()) {
    case Builtin::JsonParse: {
      this->useErrors(call->type->errType());
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *json = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_parse", ptr, {ptr, i64}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)},
          "json");
      this->lowering.refCounts().dropOperand(text);
      return this->lowering.resultLowerer().catchFailure(json, call->type, call->loc());
    }
    case Builtin::JsonStringify: {
      bool fallible = call->type->isResult();
      const Type *textType = fallible ? call->type->okType() : call->type;
      if (!fallible && call->args.size() == 1 && textType->isString() &&
          this->lowering.jsonWriters().writable(call->args[0]->type)) {
        Lowering::Operand operand = this->lowering.refCounts().emitOperand(call->args[0], true);
        llvm::Value *text =
            this->lowering.jsonWriters().emitStringify(operand.value, call->args[0]->type);
        this->lowering.refCounts().dropOperand(operand);
        return text;
      }
      if (fallible) {
        this->useErrors(call->type->errType());
      } else {
        this->lowering.builder_.CreateCall(
            this->jsonFunction("tz_json_errors", this->lowering.builder_.getVoidTy(), {ptr}),
            {llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy())});
      }
      Expr *value = call->args[0];
      bool hasSpace = call->args.size() == 2;
      Lowering::Operand operand = this->lowering.refCounts().emitOperand(
          value, !hasSpace || AssignLowerer::cannotAssign(call->args[1]));
      Lowering::Operand space{nullptr, false, Type::string()};
      llvm::Value *spaceKind = this->lowering.builder_.getInt32(0);
      llvm::Value *spaceNumber = llvm::ConstantFP::get(this->lowering.builder_.getDoubleTy(), 0.0);
      llvm::Value *spaceObj = noPath;
      llvm::Value *spaceMeta = this->lowering.builder_.getInt64(0);
      if (hasSpace && call->args[1]->type->isString()) {
        space = this->lowering.refCounts().emitOperand(call->args[1], true);
        spaceKind = this->lowering.builder_.getInt32(2);
        spaceObj = this->lowering.stringLowerer().stringObj(space.value);
        spaceMeta = this->lowering.stringLowerer().stringMeta(space.value);
      } else if (hasSpace) {
        spaceKind = this->lowering.builder_.getInt32(1);
        spaceNumber = this->lowering.exprLowerer().emitExpr(call->args[1]);
      }
      llvm::Value *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "json.text");
      llvm::Value *defined = this->lowering.builder_.CreateCall(
          this->jsonFunction("tz_json_stringify", i32,
                             {ptr, ptr, ptr, i32, this->lowering.builder_.getDoubleTy(), ptr, i64}),
          {out, this->lowering.typeDescriptors().typeDescriptor(value->type),
           this->lowering.functionFrame().spill(operand.value, "json.value"), spaceKind,
           spaceNumber, spaceObj, spaceMeta});
      this->lowering.refCounts().dropOperand(space);
      this->lowering.refCounts().dropOperand(operand);
      auto written = [&]() -> llvm::Value * {
        llvm::Value *text = this->lowering.builder_.CreateLoad(this->lowering.strTy_, out);
        if (!textType->isOptional()) {
          return text;
        }
        return this->lowering.optionalLowerer().makeOptional(
            text,
            this->lowering.builder_.CreateICmpNE(defined, this->lowering.builder_.getInt32(0)),
            textType);
      };
      if (!fallible) {
        return written();
      }
      return this->lowering.resultLowerer().catchFailure(call->type, call->loc(), written);
    }
    case Builtin::JsonIsJson:
      if (!call->args[0]->type->isUnknown()) {
        this->lowering.stmtLowerer().emitDiscarded(call->args[0]);
        return this->lowering.builder_.getInt1(call->args[0]->type->nonOptional()->isJson());
      }
      return this->lowering.unknownLowerer().emitUnknownIsJson(call->args[0]);
    case Builtin::ArrayIsArray: {
      Expr *arg = call->args[0];
      const Type *type = arg->type;
      if (type->isUnknown() || type->isAnyObject()) {
        return this->lowering.unknownLowerer().emitUnknownIsArray(arg);
      }
      if (JsonLowerer::isJsonish(type)) {
        Lowering::Operand value = this->lowering.refCounts().emitOperand(arg, true);
        llvm::Value *kind = this->lowering.builder_.CreateCall(
            this->jsonFunction("tz_json_kind", i32, {ptr}), {value.value});
        this->lowering.refCounts().dropOperand(value);
        return this->lowering.builder_.CreateICmpEQ(kind,
                                                    this->lowering.builder_.getInt32(kJsonArray));
      }
      if (const Type *u = type->nonOptional(); u->isTagged()) {
        Lowering::Operand value = this->lowering.refCounts().emitOperand(arg, true);
        llvm::Value *tag = this->lowering.unionLayout().unionTag(value.value);
        this->lowering.refCounts().dropOperand(value);
        return this->lowering.builder_.CreateICmpEQ(
            tag, this->lowering.builder_.getInt32(unsigned(UnionSlot::Array)));
      }
      bool array = type->nonOptional()->isArray() || type->nonOptional()->isTuple();
      if (array && type->isOptional()) {
        Lowering::Operand value = this->lowering.refCounts().emitOperand(arg, true);
        llvm::Value *has = this->lowering.optionalLowerer().optionalHas(value.value, type);
        this->lowering.refCounts().dropOperand(value);
        return has;
      }
      this->lowering.stmtLowerer().emitDiscarded(arg);
      return this->lowering.builder_.getInt1(array);
    }
    default:
      llvm_unreachable("not a builtin of JSON");
  }
}

}
