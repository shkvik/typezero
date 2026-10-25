#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/json/from_json_lowerer/from_json_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <map>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The kind of an array node; must match runtime/core/tzrt/tzrt.h.
constexpr int32_t kJsonArray = 4;

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

// The literal `null`, which Sema gave the type Json.
bool isNullLiteral(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && !name->var && !name->func && name->type && name->type->isJson();
}

bool isJsonish(const Type *type) { return type->nonOptional()->isJson(); }

bool mayHoldItself(const Type *type, std::vector<const Type *> &open) {
  type = type->nonOptional();
  // A Json can be put in itself (`j.self = j`), and an object of a class, or
  // one of an interface that a class implements, may refer back to itself.
  if (type->isJson() || type->isClass() || type->isBase() || type->isUnknown() ||
      type->isAnyObject() || llvm::is_contained(open, type)) {
    return true;
  }
  open.push_back(type);
  bool may = false;
  if (type->isArray()) {
    may = mayHoldItself(type->inner(), open);
  } else if (type->isResult()) {
    may = mayHoldItself(type->okType(), open) || mayHoldItself(type->errType(), open);
  } else if (type->isRecord()) { // a Map or a Set writes `{}`
    may = mayHoldItself(type->valueType(), open);
  } else if (type->isObject()) {
    may = llvm::any_of(type->fields(), [&](const Field &f) { return mayHoldItself(f.type, open); });
  } else if (type->isUnion()) {
    may = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldItself(m, open); });
  }
  open.pop_back();

  return may;
}

} // namespace

bool jsonReadMayThrow(const Expr *object) {
  if (!object->type || !object->type->nonOptional()->isJson()) {
    return false;
  }
  auto *link = dyn_cast<CastExpr>(object);

  return !link || !link->shortCircuit;
}

bool stringifyMayThrow(const Type *type) {
  std::vector<const Type *> open;

  return mayHoldItself(type, open);
}

JsonLowerer::JsonLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee JsonLowerer::jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                           llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *JsonLowerer::jsonPresent(llvm::Value *value) {
  llvm::Value *nullish = codegen_.builder_.CreateCall(
      jsonFunction("tz_json_is_nullish", codegen_.builder_.getInt32Ty(), {codegen_.builder_.getPtrTy()}), {value});

  return codegen_.builder_.CreateICmpEQ(nullish, codegen_.builder_.getInt32(0));
}

llvm::Value *JsonLowerer::emitJsonNull() {
  return codegen_.builder_.CreateCall(jsonFunction("tz_json_null", codegen_.builder_.getPtrTy(), {}), {});
}

// `j.name`: the runtime reads the key as JavaScript does.
llvm::Value *JsonLowerer::emitJsonMember(MemberExpr *member) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  CodeGen::Operand object = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *key = codegen_.stringLowerer().stringLiteral(utf16(member->name));
  llvm::Value *result =
      codegen_.builder_.CreateCall(jsonFunction("tz_json_get", ptr, {ptr, ptr, codegen_.builder_.getInt64Ty()}),
                          {object.value, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key)}, member->name);
  codegen_.refCounts().dropOperand(object);
  codegen_.emitThrowCheckIf(jsonReadMayThrow(member->object));

  return result;
}

// `j[key]` with a string key, or `j[i]` with a number.
llvm::Value *JsonLowerer::emitJsonIndex(IndexExpr *index) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  CodeGen::Operand object = codegen_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  codegen_.throwLowerer().holdOperand(object, codegen_.mayThrow().exprThrows(index->index));
  llvm::Value *result;
  if (index->index->type->isString()) {
    CodeGen::Operand key = codegen_.refCounts().emitOperand(index->index, /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(object);
    result =
        codegen_.builder_.CreateCall(jsonFunction("tz_json_get", ptr, {ptr, ptr, codegen_.builder_.getInt64Ty()}),
                            {object.value, codegen_.stringLowerer().stringObj(key.value), codegen_.stringLowerer().stringMeta(key.value)});
    codegen_.refCounts().dropOperand(key);
  } else {
    llvm::Value *position = codegen_.exprLowerer().emitExpr(index->index);
    codegen_.throwLowerer().takeOperand(object);
    result =
        codegen_.builder_.CreateCall(jsonFunction("tz_json_get_index", ptr, {ptr, codegen_.builder_.getDoubleTy()}),
                            {object.value, position});
  }
  codegen_.refCounts().dropOperand(object);
  codegen_.emitThrowCheckIf(jsonReadMayThrow(index->object));

  return result;
}

// `j.name = v`, `j[key] = v`, `j[i] = v`: as in JavaScript, the object, the
// key and the value are evaluated in that order; the runtime takes the value.
llvm::Value *JsonLowerer::emitJsonAssign(AssignExpr *assign, bool needResult) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *objectExpr = index ? index->object : member->object;
  bool later = AssignLowerer::cannotAssign(assign->value) && (!index || AssignLowerer::cannotAssign(index->index));
  CodeGen::Operand object = codegen_.refCounts().emitOperand(objectExpr, later);
  codegen_.throwLowerer().holdOperand(object, (index && codegen_.mayThrow().exprThrows(index->index)) || codegen_.mayThrow().exprThrows(assign->value));
  CodeGen::Operand key{nullptr, false, Type::string()};
  llvm::Value *position = nullptr;
  if (member) {
    key.value = codegen_.stringLowerer().stringLiteral(utf16(member->name));
  } else if (index->index->type->isString()) {
    key = codegen_.refCounts().emitOperand(index->index, AssignLowerer::cannotAssign(assign->value));
  } else {
    position = codegen_.exprLowerer().emitExpr(index->index);
  }
  codegen_.throwLowerer().holdOperand(key, codegen_.mayThrow().exprThrows(assign->value));
  llvm::Value *value = codegen_.exprLowerer().emitExpr(assign->value);
  codegen_.throwLowerer().takeOperand(object);
  codegen_.throwLowerer().takeOperand(key);
  if (needResult) {
    codegen_.refCounts().retainValue(value, assign->type);
  }
  if (position) {
    codegen_.builder_.CreateCall(
        jsonFunction("tz_json_set_index", codegen_.builder_.getVoidTy(), {ptr, codegen_.builder_.getDoubleTy(), ptr}),
        {object.value, position, value});
  } else {
    codegen_.builder_.CreateCall(jsonFunction("tz_json_set", codegen_.builder_.getVoidTy(), {ptr, ptr, i64, ptr}),
                        {object.value, codegen_.stringLowerer().stringObj(key.value), codegen_.stringLowerer().stringMeta(key.value), value});
  }
  codegen_.refCounts().dropOperand(key);
  codegen_.refCounts().dropOperand(object);
  // What is no object, or an element past the end: the value was released,
  // and so goes the reference of the result.
  codegen_.throwLowerer().emitThrowCheckWith([&] {
    if (needResult) {
      codegen_.refCounts().releaseValue(value, assign->type);
    }
  });

  return needResult ? value : nullptr;
}

llvm::Value *JsonLowerer::emitJsonConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(),
             *f64 = codegen_.builder_.getDoubleTy();
  if (isJsonish(to)) {
    if (from->isUndefined()) {
      return llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
    }
    if (from->isOptional()) {
      // `string | undefined` to `Json | undefined`: only a value that is
      // there becomes a node.
      llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
      llvm::BasicBlock *convertBlock = codegen_.functionFrame().newBlock("json.convert");
      llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("json.converted");
      codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value, from), convertBlock, endBlock);
      codegen_.functionFrame().switchTo(convertBlock);
      llvm::Value *converted =
          emitJsonConversion(codegen_.optionalLowerer().optionalValue(value, from), from->inner(), Type::json());
      llvm::BasicBlock *convertEnd = codegen_.builder_.GetInsertBlock();
      codegen_.builder_.CreateBr(endBlock);
      codegen_.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = codegen_.builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), before);
      phi->addIncoming(converted, convertEnd);
      return phi;
    }
    if (from->isJson()) {
      return value;
    }
    if (from->isBool()) {
      return codegen_.builder_.CreateCall(jsonFunction("tz_json_bool", ptr, {i32}),
                                 {codegen_.builder_.CreateZExt(value, i32)});
    }
    if (from->isInteger()) {
      value = codegen_.builder_.CreateSIToFP(value, f64);
    }
    if (from->isNumeric()) {
      return codegen_.builder_.CreateCall(jsonFunction("tz_json_number", ptr, {f64}), {value});
    }
    if (from->isString()) {
      llvm::Value *node =
          codegen_.builder_.CreateCall(jsonFunction("tz_json_string", ptr, {ptr, codegen_.builder_.getInt64Ty()}),
                              {codegen_.stringLowerer().stringObj(value), codegen_.stringLowerer().stringMeta(value)});
      codegen_.refCounts().releaseString(value);
      return node;
    }
    // A Json[]: the node takes the array over, and it stays the same array.
    return codegen_.builder_.CreateCall(jsonFunction("tz_json_from_array", ptr, {ptr}), {value});
  }

  // A Json that a check (typeof, Array.isArray) showed to hold a value of
  // `to`; it is there, whatever its type says.
  if (to->isOptional()) {
    return codegen_.optionalLowerer().makeOptional(emitJsonConversion(value, from, to->inner()), codegen_.builder_.getTrue(), to);
  }
  llvm::Value *result;
  if (to->isString()) {
    result = codegen_.stringLowerer().callReturningString(
        jsonFunction("tz_json_as_string", codegen_.builder_.getVoidTy(), {ptr, ptr}), {value});
  } else if (to->isFloat()) {
    result = codegen_.builder_.CreateCall(jsonFunction("tz_json_as_number", f64, {ptr}), {value});
  } else if (to->isBool()) {
    result = codegen_.builder_.CreateTrunc(
        codegen_.builder_.CreateCall(jsonFunction("tz_json_as_bool", i32, {ptr}), {value}),
        codegen_.builder_.getInt1Ty());
  } else {
    result = codegen_.builder_.CreateCall(jsonFunction("tz_json_as_array", ptr, {ptr}), {value});
  }
  codegen_.refCounts().releaseValue(value, from);

  return result;
}

// `==` with a Json side. Comparing with null or undefined is true for both,
// as in JavaScript; otherwise as `===`.
llvm::Value *JsonLowerer::emitJsonEquality(BinaryExpr *bin) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  bool lhsNothing = lhs->type->isUndefined() || isNullLiteral(lhs);
  bool rhsNothing = rhs->type->isUndefined() || isNullLiteral(rhs);
  if (lhsNothing || rhsNothing) {
    if (lhsNothing && rhsNothing) { // `null == undefined`
      codegen_.stmtLowerer().emitDiscarded(lhs);
      codegen_.stmtLowerer().emitDiscarded(rhs);
      return codegen_.builder_.getInt1(isEq);
    }
    // A side that is always undefined, such as `void f()`, still runs, in
    // its turn.
    if (lhs->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(lhs);
    }
    Expr *other = lhsNothing ? rhs : lhs;
    CodeGen::Operand value = codegen_.refCounts().emitOperand(other, /*canBorrow=*/true);
    if (rhs->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(rhs);
    }
    llvm::Value *present = jsonPresent(value.value);
    codegen_.refCounts().dropOperand(value);
    return isEq ? codegen_.builder_.CreateNot(present) : present;
  }
  CodeGen::Operand a = codegen_.refCounts().emitOperand(lhs, AssignLowerer::cannotAssign(rhs));
  CodeGen::Operand b = codegen_.refCounts().emitOperand(rhs, /*canBorrow=*/true);
  llvm::Value *equal;
  if (isJsonish(lhs->type) && isJsonish(rhs->type)) {
    equal = codegen_.builder_.CreateCall(jsonFunction("tz_json_strict_equal", i32, {ptr, ptr}),
                                {a.value, b.value});
  } else {
    const CodeGen::Operand &json = isJsonish(lhs->type) ? a : b;
    const CodeGen::Operand &other = isJsonish(lhs->type) ? b : a;
    if (other.type->isString()) {
      equal = codegen_.builder_.CreateCall(jsonFunction("tz_json_equals_string", i32, {ptr, ptr, i64}),
                                  {json.value, codegen_.stringLowerer().stringObj(other.value), codegen_.stringLowerer().stringMeta(other.value)});
    } else if (other.type->isBool()) {
      equal = codegen_.builder_.CreateCall(jsonFunction("tz_json_equals_bool", i32, {ptr, i32}),
                                  {json.value, codegen_.builder_.CreateZExt(other.value, i32)});
    } else {
      equal = codegen_.builder_.CreateCall(
          jsonFunction("tz_json_equals_number", i32, {ptr, codegen_.builder_.getDoubleTy()}),
          {json.value, other.value});
    }
  }
  codegen_.refCounts().dropOperand(a);
  codegen_.refCounts().dropOperand(b);
  llvm::Value *same = codegen_.builder_.CreateICmpNE(equal, codegen_.builder_.getInt32(0));

  return isEq ? same : codegen_.builder_.CreateNot(same);
}

// `j ?? fallback`: the fallback replaces null too, as in JavaScript.
llvm::Value *JsonLowerer::emitJsonNullish(BinaryExpr *bin) {
  llvm::Value *lhs = codegen_.exprLowerer().emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = codegen_.functionFrame().newBlock("nullish.default");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("nullish.end");
  codegen_.builder_.CreateCondBr(jsonPresent(lhs), valueBlock, defaultBlock);

  codegen_.functionFrame().switchTo(valueBlock);
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(defaultBlock);
  codegen_.refCounts().releaseValue(lhs, bin->lhs->type);
  llvm::Value *fallback = codegen_.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(lhs, valueBlock);
  phi->addIncoming(fallback, defaultEnd);

  return phi;
}

// `j?.name`: a Json that is null or undefined ends the chain. null is never
// freed, and undefined holds nothing, so nothing is released there.
llvm::Value *JsonLowerer::emitJsonChainLink(CastExpr *link) {
  llvm::Value *value = codegen_.exprLowerer().emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("chain.link");
  codegen_.builder_.CreateCondBr(jsonPresent(value), valueBlock, codegen_.chainEnds_.back());
  codegen_.functionFrame().switchTo(valueBlock);

  return value;
}

llvm::Value *JsonLowerer::emitJsonTypeOf(Expr *operand) {
  CodeGen::Operand value = codegen_.refCounts().emitOperand(operand, /*canBorrow=*/true);
  llvm::Value *code = codegen_.builder_.CreateCall(
      jsonFunction("tz_json_typeof", codegen_.builder_.getInt32Ty(), {codegen_.builder_.getPtrTy()}), {value.value});
  codegen_.refCounts().dropOperand(value);
  // See tz_json_typeof.
  static const std::pair<int32_t, const char16_t *> names[] = {
      {0, u"undefined"}, {2, u"boolean"}, {3, u"number"}, {4, u"string"}};
  llvm::Value *result = codegen_.stringLowerer().stringLiteral(u"object");
  for (auto [kind, name] : names) {
    result = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpEQ(code, codegen_.builder_.getInt32(kind)),
                                   codegen_.stringLowerer().stringLiteral(name), result);
  }

  return result;
}

// `{ phone: null }` where a Json is expected. A value that is undefined
// leaves its key out.
llvm::Value *JsonLowerer::emitJsonObjectLiteral(ObjectLitExpr *lit) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::FunctionCallee put = jsonFunction("tz_json_object_put", codegen_.builder_.getVoidTy(),
                                          {ptr, ptr, codegen_.builder_.getInt64Ty(), ptr});
  llvm::Value *object =
      codegen_.builder_.CreateCall(jsonFunction("tz_json_object_new", ptr, {}), {}, "json");
  // Nothing can reach the new object while its values are evaluated.
  for (const ObjectLitExpr::Property &prop : lit->props) {
    CodeGen::Operand key{codegen_.stringLowerer().stringLiteral(utf16(prop.name)), false, Type::string()};
    if (prop.key) {
      key = codegen_.refCounts().emitOperand(prop.key, AssignLowerer::cannotAssign(prop.value));
    }
    llvm::Value *value = codegen_.exprLowerer().emitExpr(prop.value);
    llvm::Value *args[] = {object, codegen_.stringLowerer().stringObj(key.value), codegen_.stringLowerer().stringMeta(key.value), value};
    if (!prop.value->type->isOptional()) {
      codegen_.builder_.CreateCall(put, args);
    } else {
      llvm::BasicBlock *putBlock = codegen_.functionFrame().newBlock("json.put");
      llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("json.next");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(value), putBlock, nextBlock);
      codegen_.functionFrame().switchTo(putBlock);
      codegen_.builder_.CreateCall(put, args);
      codegen_.builder_.CreateBr(nextBlock);
      codegen_.functionFrame().switchTo(nextBlock);
    }
    codegen_.refCounts().dropOperand(key);
  }

  return object;
}

// Object.keys, Object.values and Object.entries of a Json, and `for...in`.
llvm::Value *JsonLowerer::emitJsonObjectCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  CodeGen::Operand object = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = codegen_.builder_.CreateCall(
        jsonFunction(call->forIn ? "tz_json_for_in_keys" : "tz_json_keys", ptr, {ptr, ptr}),
        {object.value, codegen_.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result =
        codegen_.builder_.CreateCall(jsonFunction("tz_json_values", ptr, {ptr}), {object.value}, "values");
  } else {
    uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.objectLayouts().objectLayout(element).type);
    result = codegen_.builder_.CreateCall(
        jsonFunction("tz_json_entries", ptr, {ptr, ptr, ptr, codegen_.builder_.getInt64Ty()}),
        {object.value, codegen_.typeDescriptors().typeDescriptor(element), codegen_.objectLayouts().layoutDescriptor(element, {0, 1}),
         codegen_.builder_.getInt64(size)},
        "entries");
  }
  codegen_.refCounts().dropOperand(object);
  codegen_.emitThrowCheckIf(!call->forIn); // of null or undefined

  return result;
}

llvm::Value *JsonLowerer::emitJsonCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *noPath = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  switch (call->builtin) {
  case Builtin::JsonParse:
  case Builtin::JsonParseAs: {
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *json =
        codegen_.builder_.CreateCall(jsonFunction("tz_json_parse", ptr, {ptr, i64}),
                            {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)}, "json");
    codegen_.refCounts().dropOperand(text);
    codegen_.throwLowerer().emitThrowCheck(); // SyntaxError
    if (call->builtin == Builtin::JsonParse) {
      return json;
    }
    llvm::Value *result = codegen_.fromJsonLowerer().emitFromJson(call->type, json, noPath);
    codegen_.refCounts().releaseValue(json, Type::json());
    codegen_.throwLowerer().emitThrowCheck(); // a value that does not fit
    return result;
  }
  case Builtin::FromJson: {
    CodeGen::Operand json = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *result = codegen_.fromJsonLowerer().emitFromJson(call->type, json.value, noPath);
    codegen_.refCounts().dropOperand(json);
    codegen_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::EmbedJson: {
    // The text is in the program; the first evaluation parses it, and every
    // one gives that value (see tz_json_embed), for each file embedded.
    const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
    llvm::GlobalVariable *&cache = embedCaches_[cast<StringLitExpr>(call->args[1])->value];
    if (!cache) {
      cache = new llvm::GlobalVariable(
          *codegen_.module_, ptr, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), "tz.embed");
    }
    llvm::Value *literal = codegen_.stringLowerer().stringLiteral(text);
    return codegen_.builder_.CreateCall(jsonFunction("tz_json_embed", ptr, {ptr, ptr, i64}),
                               {cache, codegen_.stringLowerer().stringObj(literal), codegen_.stringLowerer().stringMeta(literal)}, "embedded");
  }
  case Builtin::JsonStringify: {
    // The runtime writes any value, by its type descriptor.
    Expr *value = call->args[0];
    bool hasSpace = call->args.size() == 2;
    CodeGen::Operand operand = codegen_.refCounts().emitOperand(value, !hasSpace || AssignLowerer::cannotAssign(call->args[1]));
    codegen_.throwLowerer().holdOperand(operand, hasSpace && codegen_.mayThrow().exprThrows(call->args[1]));
    CodeGen::Operand space{nullptr, false, Type::string()};
    llvm::Value *spaceKind = codegen_.builder_.getInt32(0);
    llvm::Value *spaceNumber = llvm::ConstantFP::get(codegen_.builder_.getDoubleTy(), 0.0);
    llvm::Value *spaceObj = noPath, *spaceMeta = codegen_.builder_.getInt64(0);
    if (hasSpace && call->args[1]->type->isString()) {
      space = codegen_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
      spaceKind = codegen_.builder_.getInt32(2);
      spaceObj = codegen_.stringLowerer().stringObj(space.value);
      spaceMeta = codegen_.stringLowerer().stringMeta(space.value);
    } else if (hasSpace) {
      spaceKind = codegen_.builder_.getInt32(1);
      spaceNumber = codegen_.exprLowerer().emitExpr(call->args[1]);
    }
    codegen_.throwLowerer().takeOperand(operand);
    llvm::Value *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "json.text");
    llvm::Value *defined =
        codegen_.builder_.CreateCall(jsonFunction("tz_json_stringify", i32,
                                         {ptr, ptr, ptr, i32, codegen_.builder_.getDoubleTy(), ptr, i64}),
                            {out, codegen_.typeDescriptors().typeDescriptor(value->type), codegen_.functionFrame().spill(operand.value, "json.value"),
                             spaceKind, spaceNumber, spaceObj, spaceMeta});
    codegen_.refCounts().dropOperand(space);
    codegen_.refCounts().dropOperand(operand);
    codegen_.emitThrowCheckIf(stringifyMayThrow(value->type)); // a value inside itself
    llvm::Value *text = codegen_.builder_.CreateLoad(codegen_.strTy_, out);
    if (!call->type->isOptional()) {
      return text;
    }
    return codegen_.optionalLowerer().makeOptional(text, codegen_.builder_.CreateICmpNE(defined, codegen_.builder_.getInt32(0)), call->type);
  }
  case Builtin::ArrayIsArray: {
    Expr *arg = call->args[0];
    const Type *type = arg->type;
    if (type->isUnknown() || type->isAnyObject()) {
      return codegen_.unknownLowerer().emitUnknownIsArray(arg);
    }
    if (isJsonish(type)) {
      CodeGen::Operand value = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *kind =
          codegen_.builder_.CreateCall(jsonFunction("tz_json_kind", i32, {ptr}), {value.value});
      codegen_.refCounts().dropOperand(value);
      return codegen_.builder_.CreateICmpEQ(kind, codegen_.builder_.getInt32(kJsonArray));
    }
    // A tagged union holds an array in its slot of arrays (codegen_union.cpp).
    if (const Type *u = type->nonOptional(); u->isTagged()) {
      CodeGen::Operand value = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *tag = codegen_.unionLayout().unionTag(value.value);
      codegen_.refCounts().dropOperand(value);
      return codegen_.builder_.CreateICmpEQ(tag, codegen_.builder_.getInt32(unsigned(UnionSlot::Array)));
    }
    // Known from the type; a tuple is an array in JavaScript.
    bool array = type->nonOptional()->isArray() || type->nonOptional()->isTuple();
    if (array && type->isOptional()) {
      CodeGen::Operand value = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *has = codegen_.optionalLowerer().optionalHas(value.value, type);
      codegen_.refCounts().dropOperand(value);
      return has;
    }
    codegen_.stmtLowerer().emitDiscarded(arg);
    return codegen_.builder_.getInt1(array);
  }
  default:
    llvm_unreachable("not a builtin of JSON");
  }
}

} // namespace tz
