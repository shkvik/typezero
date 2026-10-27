#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/json/from_json_lowerer/from_json_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <map>
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr int32_t kJsonArray = 4;

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

bool isNullLiteral(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && !name->var && !name->func && name->type && name->type->isJson();
}

bool isJsonish(const Type *type) { return type->nonOptional()->isJson(); }

}

JsonLowerer::JsonLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee JsonLowerer::jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                               llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *JsonLowerer::jsonPresent(llvm::Value *value) {
  llvm::Value *nullish = lowering_.builder_.CreateCall(
      jsonFunction("tz_json_is_nullish", lowering_.builder_.getInt32Ty(),
                   {lowering_.builder_.getPtrTy()}),
      {value});

  return lowering_.builder_.CreateICmpEQ(nullish, lowering_.builder_.getInt32(0));
}

llvm::Value *JsonLowerer::emitJsonNull() {
  return lowering_.builder_.CreateCall(
      jsonFunction("tz_json_null", lowering_.builder_.getPtrTy(), {}), {});
}

llvm::Value *JsonLowerer::emitJsonMember(MemberExpr *member) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, true);
  llvm::Value *key = lowering_.stringLowerer().stringLiteral(utf16(member->name));
  llvm::Value *result = lowering_.builder_.CreateCall(
      jsonFunction("tz_json_get", ptr, {ptr, ptr, lowering_.builder_.getInt64Ty()}),
      {object.value, lowering_.stringLowerer().stringObj(key),
       lowering_.stringLowerer().stringMeta(key)},
      member->name);
  lowering_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *JsonLowerer::emitJsonIndex(IndexExpr *index) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  Lowering::Operand object =
      lowering_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *result;
  if (index->index->type->isString()) {
    Lowering::Operand key = lowering_.refCounts().emitOperand(index->index, true);
    result = lowering_.builder_.CreateCall(
        jsonFunction("tz_json_get", ptr, {ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {object.value, lowering_.stringLowerer().stringObj(key.value),
         lowering_.stringLowerer().stringMeta(key.value)});
    lowering_.refCounts().dropOperand(key);
  } else {
    llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
    result = lowering_.builder_.CreateCall(
        jsonFunction("tz_json_get_index", ptr, {ptr, lowering_.builder_.getDoubleTy()}),
        {object.value, position});
  }
  lowering_.refCounts().dropOperand(object);

  return result;
}

llvm::Value *JsonLowerer::emitJsonAssign(AssignExpr *assign, bool needResult) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *objectExpr = index ? index->object : member->object;
  bool later = AssignLowerer::cannotAssign(assign->value) &&
               (!index || AssignLowerer::cannotAssign(index->index));
  Lowering::Operand object = lowering_.refCounts().emitOperand(objectExpr, later);
  Lowering::Operand key{nullptr, false, Type::string()};
  llvm::Value *position = nullptr;
  if (member) {
    key.value = lowering_.stringLowerer().stringLiteral(utf16(member->name));
  } else if (index->index->type->isString()) {
    key =
        lowering_.refCounts().emitOperand(index->index, AssignLowerer::cannotAssign(assign->value));
  } else {
    position = lowering_.exprLowerer().emitExpr(index->index);
  }
  llvm::Value *value = lowering_.exprLowerer().emitExpr(assign->value);
  if (needResult) {
    lowering_.refCounts().retainValue(value, assign->type);
  }
  if (position) {
    lowering_.builder_.CreateCall(jsonFunction("tz_json_set_index", lowering_.builder_.getVoidTy(),
                                               {ptr, lowering_.builder_.getDoubleTy(), ptr}),
                                  {object.value, position, value});
  } else {
    lowering_.builder_.CreateCall(
        jsonFunction("tz_json_set", lowering_.builder_.getVoidTy(), {ptr, ptr, i64, ptr}),
        {object.value, lowering_.stringLowerer().stringObj(key.value),
         lowering_.stringLowerer().stringMeta(key.value), value});
  }
  lowering_.refCounts().dropOperand(key);
  lowering_.refCounts().dropOperand(object);

  return needResult ? value : nullptr;
}

llvm::Value *JsonLowerer::emitJsonConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *f64 = lowering_.builder_.getDoubleTy();
  if (isJsonish(to)) {
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    }
    if (from->isOptional()) {
      llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
      llvm::BasicBlock *convertBlock = lowering_.functionFrame().newBlock("json.convert");
      llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("json.converted");
      lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value, from),
                                      convertBlock, endBlock);
      lowering_.functionFrame().switchTo(convertBlock);
      llvm::Value *converted = emitJsonConversion(
          lowering_.optionalLowerer().optionalValue(value, from), from->inner(), Type::json());
      llvm::BasicBlock *convertEnd = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateBr(endBlock);
      lowering_.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = lowering_.builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), before);
      phi->addIncoming(converted, convertEnd);
      return phi;
    }
    if (from->isJson()) {
      return value;
    }
    if (from->isBool()) {
      return lowering_.builder_.CreateCall(jsonFunction("tz_json_bool", ptr, {i32}),
                                           {lowering_.builder_.CreateZExt(value, i32)});
    }
    if (from->isInteger()) {
      value = lowering_.builder_.CreateSIToFP(value, f64);
    }
    if (from->isNumeric()) {
      return lowering_.builder_.CreateCall(jsonFunction("tz_json_number", ptr, {f64}), {value});
    }
    if (from->isString()) {
      llvm::Value *node = lowering_.builder_.CreateCall(
          jsonFunction("tz_json_string", ptr, {ptr, lowering_.builder_.getInt64Ty()}),
          {lowering_.stringLowerer().stringObj(value),
           lowering_.stringLowerer().stringMeta(value)});
      lowering_.refCounts().releaseString(value);
      return node;
    }
    return lowering_.builder_.CreateCall(jsonFunction("tz_json_from_array", ptr, {ptr}), {value});
  }

  if (to->isOptional()) {
    return lowering_.optionalLowerer().makeOptional(emitJsonConversion(value, from, to->inner()),
                                                    lowering_.builder_.getTrue(), to);
  }
  llvm::Value *result;
  if (to->isString()) {
    result = lowering_.stringLowerer().callReturningString(
        jsonFunction("tz_json_as_string", lowering_.builder_.getVoidTy(), {ptr, ptr}), {value});
  } else if (to->isFloat()) {
    result = lowering_.builder_.CreateCall(jsonFunction("tz_json_as_number", f64, {ptr}), {value});
  } else if (to->isBool()) {
    result = lowering_.builder_.CreateTrunc(
        lowering_.builder_.CreateCall(jsonFunction("tz_json_as_bool", i32, {ptr}), {value}),
        lowering_.builder_.getInt1Ty());
  } else {
    result = lowering_.builder_.CreateCall(jsonFunction("tz_json_as_array", ptr, {ptr}), {value});
  }
  lowering_.refCounts().releaseValue(value, from);

  return result;
}

llvm::Value *JsonLowerer::emitJsonEquality(BinaryExpr *bin) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  bool lhsNothing = lhs->type->isUndefined() || isNullLiteral(lhs);
  bool rhsNothing = rhs->type->isUndefined() || isNullLiteral(rhs);
  if (lhsNothing || rhsNothing) {
    if (lhsNothing && rhsNothing) {
      lowering_.stmtLowerer().emitDiscarded(lhs);
      lowering_.stmtLowerer().emitDiscarded(rhs);
      return lowering_.builder_.getInt1(isEq);
    }
    if (lhs->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(lhs);
    }
    Expr *other = lhsNothing ? rhs : lhs;
    Lowering::Operand value = lowering_.refCounts().emitOperand(other, true);
    if (rhs->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(rhs);
    }
    llvm::Value *present = jsonPresent(value.value);
    lowering_.refCounts().dropOperand(value);
    return isEq ? lowering_.builder_.CreateNot(present) : present;
  }
  Lowering::Operand a = lowering_.refCounts().emitOperand(lhs, AssignLowerer::cannotAssign(rhs));
  Lowering::Operand b = lowering_.refCounts().emitOperand(rhs, true);
  llvm::Value *equal;
  if (isJsonish(lhs->type) && isJsonish(rhs->type)) {
    equal = lowering_.builder_.CreateCall(jsonFunction("tz_json_strict_equal", i32, {ptr, ptr}),
                                          {a.value, b.value});
  } else {
    const Lowering::Operand &json = isJsonish(lhs->type) ? a : b;
    const Lowering::Operand &other = isJsonish(lhs->type) ? b : a;
    if (other.type->isString()) {
      equal = lowering_.builder_.CreateCall(
          jsonFunction("tz_json_equals_string", i32, {ptr, ptr, i64}),
          {json.value, lowering_.stringLowerer().stringObj(other.value),
           lowering_.stringLowerer().stringMeta(other.value)});
    } else if (other.type->isBool()) {
      equal = lowering_.builder_.CreateCall(
          jsonFunction("tz_json_equals_bool", i32, {ptr, i32}),
          {json.value, lowering_.builder_.CreateZExt(other.value, i32)});
    } else {
      equal = lowering_.builder_.CreateCall(
          jsonFunction("tz_json_equals_number", i32, {ptr, lowering_.builder_.getDoubleTy()}),
          {json.value, other.value});
    }
  }
  lowering_.refCounts().dropOperand(a);
  lowering_.refCounts().dropOperand(b);
  llvm::Value *same = lowering_.builder_.CreateICmpNE(equal, lowering_.builder_.getInt32(0));

  return isEq ? same : lowering_.builder_.CreateNot(same);
}

llvm::Value *JsonLowerer::emitJsonNullish(BinaryExpr *bin) {
  llvm::Value *lhs = lowering_.exprLowerer().emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = lowering_.functionFrame().newBlock("nullish.default");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("nullish.end");
  lowering_.builder_.CreateCondBr(jsonPresent(lhs), valueBlock, defaultBlock);

  lowering_.functionFrame().switchTo(valueBlock);
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(defaultBlock);
  lowering_.refCounts().releaseValue(lhs, bin->lhs->type);
  llvm::Value *fallback = lowering_.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(lhs, valueBlock);
  phi->addIncoming(fallback, defaultEnd);

  return phi;
}

llvm::Value *JsonLowerer::emitJsonChainLink(CastExpr *link) {
  llvm::Value *value = lowering_.exprLowerer().emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("chain.link");
  lowering_.builder_.CreateCondBr(jsonPresent(value), valueBlock, lowering_.chainEnds_.back());
  lowering_.functionFrame().switchTo(valueBlock);

  return value;
}

llvm::Value *JsonLowerer::emitJsonTypeOf(Expr *operand) {
  Lowering::Operand value = lowering_.refCounts().emitOperand(operand, true);
  llvm::Value *code =
      lowering_.builder_.CreateCall(jsonFunction("tz_json_typeof", lowering_.builder_.getInt32Ty(),
                                                 {lowering_.builder_.getPtrTy()}),
                                    {value.value});
  lowering_.refCounts().dropOperand(value);
  static const std::pair<int32_t, const char16_t *> names[] = {
      {0, u"undefined"}, {2, u"boolean"}, {3, u"number"}, {4, u"string"}};
  llvm::Value *result = lowering_.stringLowerer().stringLiteral(u"object");
  for (auto [kind, name] : names) {
    result = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpEQ(code, lowering_.builder_.getInt32(kind)),
        lowering_.stringLowerer().stringLiteral(name), result);
  }

  return result;
}

llvm::Value *JsonLowerer::emitJsonObjectLiteral(ObjectLitExpr *lit) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::FunctionCallee put = jsonFunction("tz_json_object_put", lowering_.builder_.getVoidTy(),
                                          {ptr, ptr, lowering_.builder_.getInt64Ty(), ptr});
  llvm::Value *object =
      lowering_.builder_.CreateCall(jsonFunction("tz_json_object_new", ptr, {}), {}, "json");
  for (const ObjectLitExpr::Property &prop : lit->props) {
    Lowering::Operand key{lowering_.stringLowerer().stringLiteral(utf16(prop.name)), false,
                          Type::string()};
    if (prop.key) {
      key = lowering_.refCounts().emitOperand(prop.key, AssignLowerer::cannotAssign(prop.value));
    }
    llvm::Value *value = lowering_.exprLowerer().emitExpr(prop.value);
    llvm::Value *args[] = {object, lowering_.stringLowerer().stringObj(key.value),
                           lowering_.stringLowerer().stringMeta(key.value), value};
    if (!prop.value->type->isOptional()) {
      lowering_.builder_.CreateCall(put, args);
    } else {
      llvm::BasicBlock *putBlock = lowering_.functionFrame().newBlock("json.put");
      llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("json.next");
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(value), putBlock,
                                      nextBlock);
      lowering_.functionFrame().switchTo(putBlock);
      lowering_.builder_.CreateCall(put, args);
      lowering_.builder_.CreateBr(nextBlock);
      lowering_.functionFrame().switchTo(nextBlock);
    }
    lowering_.refCounts().dropOperand(key);
  }

  return object;
}

llvm::Value *JsonLowerer::emitJsonObjectCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  Lowering::Operand object = lowering_.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = lowering_.builder_.CreateCall(
        jsonFunction(call->forIn ? "tz_json_for_in_keys" : "tz_json_keys", ptr, {ptr, ptr}),
        {object.value, lowering_.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = lowering_.builder_.CreateCall(jsonFunction("tz_json_values", ptr, {ptr}),
                                           {object.value}, "values");
  } else {
    uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(
        lowering_.objectLayouts().objectLayout(element).type);
    result = lowering_.builder_.CreateCall(
        jsonFunction("tz_json_entries", ptr, {ptr, ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {object.value, lowering_.typeDescriptors().typeDescriptor(element),
         lowering_.objectLayouts().layoutDescriptor(element, {0, 1}),
         lowering_.builder_.getInt64(size)},
        "entries");
  }
  lowering_.refCounts().dropOperand(object);

  return result;
}

void JsonLowerer::useErrors(const Type *errorType) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message", "path"}}};
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      jsonFunction("tz_json_errors", lowering_.builder_.getVoidTy(), {ptr}),
      {lowering_.objectMaker().maker(errorType, orders)});
}

llvm::Value *JsonLowerer::emitJsonCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *noPath = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  switch (call->builtin) {
  case Builtin::JsonParse:
  case Builtin::JsonParseAs: {
    useErrors(call->type->errType());
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *json =
        lowering_.builder_.CreateCall(jsonFunction("tz_json_parse", ptr, {ptr, i64}),
                                      {lowering_.stringLowerer().stringObj(text.value),
                                       lowering_.stringLowerer().stringMeta(text.value)},
                                      "json");
    lowering_.refCounts().dropOperand(text);
    if (call->builtin == Builtin::JsonParse) {
      return lowering_.resultLowerer().catchFailure(json, call->type, call->loc());
    }
    llvm::Value *result =
        lowering_.fromJsonLowerer().emitFromJsonOrSkip(call->type->okType(), json, noPath);
    lowering_.refCounts().releaseValue(json, Type::json());
    return lowering_.resultLowerer().catchFailure(result, call->type, call->loc());
  }
  case Builtin::FromJson: {
    useErrors(call->type->errType());
    Lowering::Operand json = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *result =
        lowering_.fromJsonLowerer().emitFromJson(call->type->okType(), json.value, noPath);
    lowering_.refCounts().dropOperand(json);
    return lowering_.resultLowerer().catchFailure(result, call->type, call->loc());
  }
  case Builtin::EmbedJson: {
    const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
    llvm::GlobalVariable *&cache = embedCaches_[cast<StringLitExpr>(call->args[1])->value];
    if (!cache) {
      cache = new llvm::GlobalVariable(
          *lowering_.module_, ptr, false, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), "tz.embed");
    }
    llvm::Value *literal = lowering_.stringLowerer().stringLiteral(text);
    return lowering_.builder_.CreateCall(jsonFunction("tz_json_embed", ptr, {ptr, ptr, i64}),
                                         {cache, lowering_.stringLowerer().stringObj(literal),
                                          lowering_.stringLowerer().stringMeta(literal)},
                                         "embedded");
  }
  case Builtin::JsonStringify: {
    bool fallible = call->type->isResult();
    const Type *textType = fallible ? call->type->okType() : call->type;
    if (fallible) {
      useErrors(call->type->errType());
    } else {
      lowering_.builder_.CreateCall(
          jsonFunction("tz_json_errors", lowering_.builder_.getVoidTy(), {ptr}),
          {llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy())});
    }
    Expr *value = call->args[0];
    bool hasSpace = call->args.size() == 2;
    Lowering::Operand operand = lowering_.refCounts().emitOperand(
        value, !hasSpace || AssignLowerer::cannotAssign(call->args[1]));
    Lowering::Operand space{nullptr, false, Type::string()};
    llvm::Value *spaceKind = lowering_.builder_.getInt32(0);
    llvm::Value *spaceNumber = llvm::ConstantFP::get(lowering_.builder_.getDoubleTy(), 0.0);
    llvm::Value *spaceObj = noPath, *spaceMeta = lowering_.builder_.getInt64(0);
    if (hasSpace && call->args[1]->type->isString()) {
      space = lowering_.refCounts().emitOperand(call->args[1], true);
      spaceKind = lowering_.builder_.getInt32(2);
      spaceObj = lowering_.stringLowerer().stringObj(space.value);
      spaceMeta = lowering_.stringLowerer().stringMeta(space.value);
    } else if (hasSpace) {
      spaceKind = lowering_.builder_.getInt32(1);
      spaceNumber = lowering_.exprLowerer().emitExpr(call->args[1]);
    }
    llvm::Value *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "json.text");
    llvm::Value *defined = lowering_.builder_.CreateCall(
        jsonFunction("tz_json_stringify", i32,
                     {ptr, ptr, ptr, i32, lowering_.builder_.getDoubleTy(), ptr, i64}),
        {out, lowering_.typeDescriptors().typeDescriptor(value->type),
         lowering_.functionFrame().spill(operand.value, "json.value"), spaceKind, spaceNumber,
         spaceObj, spaceMeta});
    lowering_.refCounts().dropOperand(space);
    lowering_.refCounts().dropOperand(operand);
    auto written = [&]() -> llvm::Value * {
      llvm::Value *text = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
      if (!textType->isOptional()) {
        return text;
      }
      return lowering_.optionalLowerer().makeOptional(
          text, lowering_.builder_.CreateICmpNE(defined, lowering_.builder_.getInt32(0)), textType);
    };
    if (!fallible) {
      return written();
    }
    return lowering_.resultLowerer().catchFailure(call->type, call->loc(), written);
  }
  case Builtin::ArrayIsArray: {
    Expr *arg = call->args[0];
    const Type *type = arg->type;
    if (type->isUnknown() || type->isAnyObject()) {
      return lowering_.unknownLowerer().emitUnknownIsArray(arg);
    }
    if (isJsonish(type)) {
      Lowering::Operand value = lowering_.refCounts().emitOperand(arg, true);
      llvm::Value *kind =
          lowering_.builder_.CreateCall(jsonFunction("tz_json_kind", i32, {ptr}), {value.value});
      lowering_.refCounts().dropOperand(value);
      return lowering_.builder_.CreateICmpEQ(kind, lowering_.builder_.getInt32(kJsonArray));
    }
    if (const Type *u = type->nonOptional(); u->isTagged()) {
      Lowering::Operand value = lowering_.refCounts().emitOperand(arg, true);
      llvm::Value *tag = lowering_.unionLayout().unionTag(value.value);
      lowering_.refCounts().dropOperand(value);
      return lowering_.builder_.CreateICmpEQ(
          tag, lowering_.builder_.getInt32(unsigned(UnionSlot::Array)));
    }
    bool array = type->nonOptional()->isArray() || type->nonOptional()->isTuple();
    if (array && type->isOptional()) {
      Lowering::Operand value = lowering_.refCounts().emitOperand(arg, true);
      llvm::Value *has = lowering_.optionalLowerer().optionalHas(value.value, type);
      lowering_.refCounts().dropOperand(value);
      return has;
    }
    lowering_.stmtLowerer().emitDiscarded(arg);
    return lowering_.builder_.getInt1(array);
  }
  default:
    llvm_unreachable("not a builtin of JSON");
  }
}

}
