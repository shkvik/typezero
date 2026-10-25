// Json values (docs/modules.md, section 5): reading and writing keys,
// conversions, equality, `??` and `?.` that take null for undefined, literals,
// JSON.parse, JSON.stringify, embedJson, and the functions CodeGen makes for
// JSON.parse<T> and fromJson<T> (docs/narrowing.md, section 3).
//
// A Json is a pointer to a node of the runtime
// (runtime/library/json/json/json.c), counted as objects are, so retaining one
// is the inline increment of objects and arrays; `Json | undefined` is that
// pointer, null for undefined. The runtime does what depends on the kind of the
// node.
//
// What JavaScript throws, the runtime throws (docs/errors.md): reading a key
// of null or undefined, writing one of what is no object, a text that is not
// JSON, a value that does not fit T, a value inside itself. The program
// checks tz_thrown after those calls, and ThrowScanner (codegen_throw.cpp)
// knows them.

#include "compiler/lower/codegen/codegen.h"

#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"

#include <map>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

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
      type->isAnyObject() || llvm::is_contained(open, type))
    return true;
  open.push_back(type);
  bool may = false;
  if (type->isArray())
    may = mayHoldItself(type->inner(), open);
  else if (type->isResult())
    may = mayHoldItself(type->okType(), open) || mayHoldItself(type->errType(), open);
  else if (type->isRecord()) // a Map or a Set writes `{}`
    may = mayHoldItself(type->valueType(), open);
  else if (type->isObject())
    may = llvm::any_of(type->fields(), [&](const Field &f) { return mayHoldItself(f.type, open); });
  else if (type->isUnion())
    may = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldItself(m, open); });
  open.pop_back();
  return may;
}

} // namespace

bool jsonReadMayThrow(const Expr *object) {
  if (!object->type || !object->type->nonOptional()->isJson())
    return false;
  auto *link = dyn_cast<CastExpr>(object);
  return !link || !link->shortCircuit;
}

bool stringifyMayThrow(const Type *type) {
  std::vector<const Type *> open;
  return mayHoldItself(type, open);
}

llvm::FunctionCallee CodeGen::jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                           llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::jsonPresent(llvm::Value *value) {
  llvm::Value *nullish = builder_.CreateCall(
      jsonFunction("tz_json_is_nullish", builder_.getInt32Ty(), {builder_.getPtrTy()}), {value});
  return builder_.CreateICmpEQ(nullish, builder_.getInt32(0));
}

llvm::Value *CodeGen::emitJsonNull() {
  return builder_.CreateCall(jsonFunction("tz_json_null", builder_.getPtrTy(), {}), {});
}

//===----------------------------------------------------------------------===//
// Keys
//===----------------------------------------------------------------------===//

// `j.name`: the runtime reads the key as JavaScript does.
llvm::Value *CodeGen::emitJsonMember(MemberExpr *member) {
  llvm::Type *ptr = builder_.getPtrTy();
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *key = stringLiteral(utf16(member->name));
  llvm::Value *result =
      builder_.CreateCall(jsonFunction("tz_json_get", ptr, {ptr, ptr, builder_.getInt64Ty()}),
                          {object.value, stringObj(key), stringMeta(key)}, member->name);
  dropOperand(object);
  emitThrowCheckIf(jsonReadMayThrow(member->object));
  return result;
}

// `j[key]` with a string key, or `j[i]` with a number.
llvm::Value *CodeGen::emitJsonIndex(IndexExpr *index) {
  llvm::Type *ptr = builder_.getPtrTy();
  Operand object = emitOperand(index->object, cannotAssign(index->index));
  holdOperand(object, exprThrows(index->index));
  llvm::Value *result;
  if (index->index->type->isString()) {
    Operand key = emitOperand(index->index, /*canBorrow=*/true);
    takeOperand(object);
    result =
        builder_.CreateCall(jsonFunction("tz_json_get", ptr, {ptr, ptr, builder_.getInt64Ty()}),
                            {object.value, stringObj(key.value), stringMeta(key.value)});
    dropOperand(key);
  } else {
    llvm::Value *position = emitExpr(index->index);
    takeOperand(object);
    result =
        builder_.CreateCall(jsonFunction("tz_json_get_index", ptr, {ptr, builder_.getDoubleTy()}),
                            {object.value, position});
  }
  dropOperand(object);
  emitThrowCheckIf(jsonReadMayThrow(index->object));
  return result;
}

// `j.name = v`, `j[key] = v`, `j[i] = v`: as in JavaScript, the object, the
// key and the value are evaluated in that order; the runtime takes the value.
llvm::Value *CodeGen::emitJsonAssign(AssignExpr *assign, bool needResult) {
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *objectExpr = index ? index->object : member->object;
  bool later = cannotAssign(assign->value) && (!index || cannotAssign(index->index));
  Operand object = emitOperand(objectExpr, later);
  holdOperand(object, (index && exprThrows(index->index)) || exprThrows(assign->value));
  Operand key{nullptr, false, Type::string()};
  llvm::Value *position = nullptr;
  if (member)
    key.value = stringLiteral(utf16(member->name));
  else if (index->index->type->isString())
    key = emitOperand(index->index, cannotAssign(assign->value));
  else
    position = emitExpr(index->index);
  holdOperand(key, exprThrows(assign->value));
  llvm::Value *value = emitExpr(assign->value);
  takeOperand(object);
  takeOperand(key);
  if (needResult)
    retainValue(value, assign->type);
  if (position)
    builder_.CreateCall(
        jsonFunction("tz_json_set_index", builder_.getVoidTy(), {ptr, builder_.getDoubleTy(), ptr}),
        {object.value, position, value});
  else
    builder_.CreateCall(jsonFunction("tz_json_set", builder_.getVoidTy(), {ptr, ptr, i64, ptr}),
                        {object.value, stringObj(key.value), stringMeta(key.value), value});
  dropOperand(key);
  dropOperand(object);
  // What is no object, or an element past the end: the value was released,
  // and so goes the reference of the result.
  emitThrowCheckWith([&] {
    if (needResult)
      releaseValue(value, assign->type);
  });
  return needResult ? value : nullptr;
}

//===----------------------------------------------------------------------===//
// Conversions, equality, `??`, `?.` and typeof
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitJsonConversion(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(),
             *f64 = builder_.getDoubleTy();
  if (isJsonish(to)) {
    if (from->isUndefined())
      return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    if (from->isOptional()) {
      // `string | undefined` to `Json | undefined`: only a value that is
      // there becomes a node.
      llvm::BasicBlock *before = builder_.GetInsertBlock();
      llvm::BasicBlock *convertBlock = newBlock("json.convert");
      llvm::BasicBlock *endBlock = newBlock("json.converted");
      builder_.CreateCondBr(optionalHas(value, from), convertBlock, endBlock);
      switchTo(convertBlock);
      llvm::Value *converted =
          emitJsonConversion(optionalValue(value, from), from->inner(), Type::json());
      llvm::BasicBlock *convertEnd = builder_.GetInsertBlock();
      builder_.CreateBr(endBlock);
      switchTo(endBlock);
      llvm::PHINode *phi = builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()), before);
      phi->addIncoming(converted, convertEnd);
      return phi;
    }
    if (from->isJson())
      return value;
    if (from->isBool())
      return builder_.CreateCall(jsonFunction("tz_json_bool", ptr, {i32}),
                                 {builder_.CreateZExt(value, i32)});
    if (from->isInteger())
      value = builder_.CreateSIToFP(value, f64);
    if (from->isNumeric())
      return builder_.CreateCall(jsonFunction("tz_json_number", ptr, {f64}), {value});
    if (from->isString()) {
      llvm::Value *node = builder_.CreateCall(
          jsonFunction("tz_json_string", ptr, {ptr, builder_.getInt64Ty()}),
          {stringObj(value), stringMeta(value)});
      releaseString(value);
      return node;
    }
    // A Json[]: the node takes the array over, and it stays the same array.
    return builder_.CreateCall(jsonFunction("tz_json_from_array", ptr, {ptr}), {value});
  }

  // A Json that a check (typeof, Array.isArray) showed to hold a value of
  // `to`; it is there, whatever its type says.
  if (to->isOptional())
    return makeOptional(emitJsonConversion(value, from, to->inner()), builder_.getTrue(), to);
  llvm::Value *result;
  if (to->isString())
    result = callReturningString(
        jsonFunction("tz_json_as_string", builder_.getVoidTy(), {ptr, ptr}), {value});
  else if (to->isFloat())
    result = builder_.CreateCall(jsonFunction("tz_json_as_number", f64, {ptr}), {value});
  else if (to->isBool())
    result = builder_.CreateTrunc(
        builder_.CreateCall(jsonFunction("tz_json_as_bool", i32, {ptr}), {value}),
        builder_.getInt1Ty());
  else
    result = builder_.CreateCall(jsonFunction("tz_json_as_array", ptr, {ptr}), {value});
  releaseValue(value, from);
  return result;
}

// `==` with a Json side. Comparing with null or undefined is true for both,
// as in JavaScript; otherwise as `===`.
llvm::Value *CodeGen::emitJsonEquality(BinaryExpr *bin) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  bool lhsNothing = lhs->type->isUndefined() || isNullLiteral(lhs);
  bool rhsNothing = rhs->type->isUndefined() || isNullLiteral(rhs);
  if (lhsNothing || rhsNothing) {
    if (lhsNothing && rhsNothing) { // `null == undefined`
      emitDiscarded(lhs);
      emitDiscarded(rhs);
      return builder_.getInt1(isEq);
    }
    // A side that is always undefined, such as `void f()`, still runs, in
    // its turn.
    if (lhs->type->isUndefined())
      emitDiscarded(lhs);
    Expr *other = lhsNothing ? rhs : lhs;
    Operand value = emitOperand(other, /*canBorrow=*/true);
    if (rhs->type->isUndefined())
      emitDiscarded(rhs);
    llvm::Value *present = jsonPresent(value.value);
    dropOperand(value);
    return isEq ? builder_.CreateNot(present) : present;
  }
  Operand a = emitOperand(lhs, cannotAssign(rhs));
  Operand b = emitOperand(rhs, /*canBorrow=*/true);
  llvm::Value *equal;
  if (isJsonish(lhs->type) && isJsonish(rhs->type)) {
    equal = builder_.CreateCall(jsonFunction("tz_json_strict_equal", i32, {ptr, ptr}),
                                {a.value, b.value});
  } else {
    const Operand &json = isJsonish(lhs->type) ? a : b;
    const Operand &other = isJsonish(lhs->type) ? b : a;
    if (other.type->isString())
      equal = builder_.CreateCall(jsonFunction("tz_json_equals_string", i32, {ptr, ptr, i64}),
                                  {json.value, stringObj(other.value), stringMeta(other.value)});
    else if (other.type->isBool())
      equal = builder_.CreateCall(jsonFunction("tz_json_equals_bool", i32, {ptr, i32}),
                                  {json.value, builder_.CreateZExt(other.value, i32)});
    else
      equal = builder_.CreateCall(
          jsonFunction("tz_json_equals_number", i32, {ptr, builder_.getDoubleTy()}),
          {json.value, other.value});
  }
  dropOperand(a);
  dropOperand(b);
  llvm::Value *same = builder_.CreateICmpNE(equal, builder_.getInt32(0));
  return isEq ? same : builder_.CreateNot(same);
}

// `j ?? fallback`: the fallback replaces null too, as in JavaScript.
llvm::Value *CodeGen::emitJsonNullish(BinaryExpr *bin) {
  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = newBlock("nullish.default");
  llvm::BasicBlock *endBlock = newBlock("nullish.end");
  builder_.CreateCondBr(jsonPresent(lhs), valueBlock, defaultBlock);

  switchTo(valueBlock);
  builder_.CreateBr(endBlock);

  switchTo(defaultBlock);
  releaseValue(lhs, bin->lhs->type);
  llvm::Value *fallback = emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(bin->type), 2);
  phi->addIncoming(lhs, valueBlock);
  phi->addIncoming(fallback, defaultEnd);
  return phi;
}

// `j?.name`: a Json that is null or undefined ends the chain. null is never
// freed, and undefined holds nothing, so nothing is released there.
llvm::Value *CodeGen::emitJsonChainLink(CastExpr *link) {
  llvm::Value *value = emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = newBlock("chain.link");
  builder_.CreateCondBr(jsonPresent(value), valueBlock, chainEnds_.back());
  switchTo(valueBlock);
  return value;
}

llvm::Value *CodeGen::emitJsonTypeOf(Expr *operand) {
  Operand value = emitOperand(operand, /*canBorrow=*/true);
  llvm::Value *code = builder_.CreateCall(
      jsonFunction("tz_json_typeof", builder_.getInt32Ty(), {builder_.getPtrTy()}), {value.value});
  dropOperand(value);
  // See tz_json_typeof.
  static const std::pair<int32_t, const char16_t *> names[] = {
      {0, u"undefined"}, {2, u"boolean"}, {3, u"number"}, {4, u"string"}};
  llvm::Value *result = stringLiteral(u"object");
  for (auto [kind, name] : names)
    result = builder_.CreateSelect(builder_.CreateICmpEQ(code, builder_.getInt32(kind)),
                                   stringLiteral(name), result);
  return result;
}

//===----------------------------------------------------------------------===//
// Literals and Object.keys
//===----------------------------------------------------------------------===//

// `{ phone: null }` where a Json is expected. A value that is undefined
// leaves its key out.
llvm::Value *CodeGen::emitJsonObjectLiteral(ObjectLitExpr *lit) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::FunctionCallee put = jsonFunction("tz_json_object_put", builder_.getVoidTy(),
                                          {ptr, ptr, builder_.getInt64Ty(), ptr});
  llvm::Value *object =
      builder_.CreateCall(jsonFunction("tz_json_object_new", ptr, {}), {}, "json");
  // Nothing can reach the new object while its values are evaluated.
  for (const ObjectLitExpr::Property &prop : lit->props) {
    Operand key{stringLiteral(utf16(prop.name)), false, Type::string()};
    if (prop.key)
      key = emitOperand(prop.key, cannotAssign(prop.value));
    llvm::Value *value = emitExpr(prop.value);
    llvm::Value *args[] = {object, stringObj(key.value), stringMeta(key.value), value};
    if (!prop.value->type->isOptional()) {
      builder_.CreateCall(put, args);
    } else {
      llvm::BasicBlock *putBlock = newBlock("json.put");
      llvm::BasicBlock *nextBlock = newBlock("json.next");
      builder_.CreateCondBr(builder_.CreateIsNotNull(value), putBlock, nextBlock);
      switchTo(putBlock);
      builder_.CreateCall(put, args);
      builder_.CreateBr(nextBlock);
      switchTo(nextBlock);
    }
    dropOperand(key);
  }
  return object;
}

// Object.keys, Object.values and Object.entries of a Json, and `for...in`.
llvm::Value *CodeGen::emitJsonObjectCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy();
  Operand object = emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = builder_.CreateCall(
        jsonFunction(call->forIn ? "tz_json_for_in_keys" : "tz_json_keys", ptr, {ptr, ptr}),
        {object.value, typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result =
        builder_.CreateCall(jsonFunction("tz_json_values", ptr, {ptr}), {object.value}, "values");
  } else {
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectLayout(element).type);
    result = builder_.CreateCall(
        jsonFunction("tz_json_entries", ptr, {ptr, ptr, ptr, builder_.getInt64Ty()}),
        {object.value, typeDescriptor(element), layoutDescriptor(element, {0, 1}),
         builder_.getInt64(size)},
        "entries");
  }
  dropOperand(object);
  emitThrowCheckIf(!call->forIn); // of null or undefined
  return result;
}

//===----------------------------------------------------------------------===//
// JSON, embedJson and Array.isArray
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitJsonCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Value *noPath = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  switch (call->builtin) {
  case Builtin::JsonParse:
  case Builtin::JsonParseAs: {
    Operand text = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *json =
        builder_.CreateCall(jsonFunction("tz_json_parse", ptr, {ptr, i64}),
                            {stringObj(text.value), stringMeta(text.value)}, "json");
    dropOperand(text);
    emitThrowCheck(); // SyntaxError
    if (call->builtin == Builtin::JsonParse)
      return json;
    llvm::Value *result = emitFromJson(call->type, json, noPath);
    releaseValue(json, Type::json());
    emitThrowCheck(); // a value that does not fit
    return result;
  }
  case Builtin::FromJson: {
    Operand json = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *result = emitFromJson(call->type, json.value, noPath);
    dropOperand(json);
    emitThrowCheck();
    return result;
  }
  case Builtin::EmbedJson: {
    // The text is in the program; the first evaluation parses it, and every
    // one gives that value (see tz_json_embed), for each file embedded.
    const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
    llvm::GlobalVariable *&cache = embedCaches_[cast<StringLitExpr>(call->args[1])->value];
    if (!cache)
      cache = new llvm::GlobalVariable(
          *module_, ptr, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantPointerNull::get(builder_.getPtrTy()), "tz.embed");
    llvm::Value *literal = stringLiteral(text);
    return builder_.CreateCall(jsonFunction("tz_json_embed", ptr, {ptr, ptr, i64}),
                               {cache, stringObj(literal), stringMeta(literal)}, "embedded");
  }
  case Builtin::JsonStringify: {
    // The runtime writes any value, by its type descriptor.
    Expr *value = call->args[0];
    bool hasSpace = call->args.size() == 2;
    Operand operand = emitOperand(value, !hasSpace || cannotAssign(call->args[1]));
    holdOperand(operand, hasSpace && exprThrows(call->args[1]));
    Operand space{nullptr, false, Type::string()};
    llvm::Value *spaceKind = builder_.getInt32(0);
    llvm::Value *spaceNumber = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
    llvm::Value *spaceObj = noPath, *spaceMeta = builder_.getInt64(0);
    if (hasSpace && call->args[1]->type->isString()) {
      space = emitOperand(call->args[1], /*canBorrow=*/true);
      spaceKind = builder_.getInt32(2);
      spaceObj = stringObj(space.value);
      spaceMeta = stringMeta(space.value);
    } else if (hasSpace) {
      spaceKind = builder_.getInt32(1);
      spaceNumber = emitExpr(call->args[1]);
    }
    takeOperand(operand);
    llvm::Value *out = createStackSlot(strTy_, "json.text");
    llvm::Value *defined = builder_.CreateCall(
        jsonFunction("tz_json_stringify", i32,
                     {ptr, ptr, ptr, i32, builder_.getDoubleTy(), ptr, i64}),
        {out, typeDescriptor(value->type), spill(operand.value, "json.value"), spaceKind,
         spaceNumber, spaceObj, spaceMeta});
    dropOperand(space);
    dropOperand(operand);
    emitThrowCheckIf(stringifyMayThrow(value->type)); // a value inside itself
    llvm::Value *text = builder_.CreateLoad(strTy_, out);
    if (!call->type->isOptional())
      return text;
    return makeOptional(text, builder_.CreateICmpNE(defined, builder_.getInt32(0)), call->type);
  }
  case Builtin::ArrayIsArray: {
    Expr *arg = call->args[0];
    const Type *type = arg->type;
    if (type->isUnknown() || type->isAnyObject())
      return emitUnknownIsArray(arg);
    if (isJsonish(type)) {
      Operand value = emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *kind =
          builder_.CreateCall(jsonFunction("tz_json_kind", i32, {ptr}), {value.value});
      dropOperand(value);
      return builder_.CreateICmpEQ(kind, builder_.getInt32(kJsonArray));
    }
    // A tagged union holds an array in its slot of arrays (codegen_union.cpp).
    if (const Type *u = type->nonOptional(); u->isTagged()) {
      Operand value = emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *tag = unionTag(value.value);
      dropOperand(value);
      return builder_.CreateICmpEQ(tag, builder_.getInt32(unsigned(UnionSlot::Array)));
    }
    // Known from the type; a tuple is an array in JavaScript.
    bool array = type->nonOptional()->isArray() || type->nonOptional()->isTuple();
    if (array && type->isOptional()) {
      Operand value = emitOperand(arg, /*canBorrow=*/true);
      llvm::Value *has = optionalHas(value.value, type);
      dropOperand(value);
      return has;
    }
    emitDiscarded(arg);
    return builder_.getInt1(array);
  }
  default:
    llvm_unreachable("not a builtin of JSON");
  }
}

//===----------------------------------------------------------------------===//
// JSON.parse<T> and fromJson<T>
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *voidTy = builder_.getVoidTy();
  switch (type->kind()) {
  case Type::Kind::Int:
    return builder_.CreateCall(jsonFunction("tz_json_expect_int", i32, {ptr, ptr}), {json, path});
  case Type::Kind::Long:
    return builder_.CreateCall(
        jsonFunction("tz_json_expect_long", builder_.getInt64Ty(), {ptr, ptr}), {json, path});
  case Type::Kind::Double:
    return builder_.CreateCall(
        jsonFunction("tz_json_expect_double", builder_.getDoubleTy(), {ptr, ptr}), {json, path});
  case Type::Kind::Bool:
    return builder_.CreateTrunc(
        builder_.CreateCall(jsonFunction("tz_json_expect_bool", i32, {ptr, ptr}), {json, path}),
        builder_.getInt1Ty());
  case Type::Kind::String:
    return callReturningString(jsonFunction("tz_json_expect_string", voidTy, {ptr, ptr, ptr}),
                               {json, path});
  case Type::Kind::Decimal: {
    llvm::AllocaInst *out = createStackSlot(decTy_, "dec.out");
    builder_.CreateCall(jsonFunction("tz_json_expect_decimal", voidTy, {ptr, ptr, ptr}),
                        {out, json, path});
    return builder_.CreateLoad(decTy_, out);
  }
  case Type::Kind::Json:
    return builder_.CreateCall(jsonFunction("tz_json_expect_value", ptr, {ptr, ptr}), {json, path});
  case Type::Kind::Optional: {
    // A key that is not there gives undefined, and so does null, except for
    // a Json, which holds null.
    const Type *inner = type->inner();
    llvm::Value *present = inner->isJson() ? builder_.CreateIsNotNull(json) : jsonPresent(json);
    llvm::BasicBlock *before = builder_.GetInsertBlock();
    llvm::BasicBlock *valueBlock = newBlock("json.value");
    llvm::BasicBlock *endBlock = newBlock("json.optional");
    builder_.CreateCondBr(present, valueBlock, endBlock);
    switchTo(valueBlock);
    llvm::Value *value = makeOptional(emitFromJson(inner, json, path), builder_.getTrue(), type);
    llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(type), 2);
    phi->addIncoming(undefinedValue(type), before);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::Literal:
  case Type::Kind::Union:
    // An object of a class, through its constructor (fromJson(cls, j)).
    if (const ClassDecl *cls = type->classDecl(); cls && !cls->literal)
      return builder_.CreateCall(classFromJsonFunction(cls), {json, path});
    return builder_.CreateCall(fromJsonFunction(type), {json, path});
  default:
    llvm_unreachable("Sema checked that JSON.parse<T> can build the type");
  }
}

// `T tz.fromJson.T(ptr json, ptr path)` for an array, an object, a literal
// type or a union. It is emitted aside, while the function that needs it is
// being emitted. A value that does not fit throws: the function releases
// what it built and gives null, which its caller drops when it sees
// tz_thrown.
llvm::Function *CodeGen::fromJsonFunction(const Type *type) {
  if (llvm::Function *fn = fromJsonFunctions_.lookup(type))
    return fn;
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  if (!jsonPathTy_) // runtime/library/json/json/json.c: TzJsonPath
    jsonPathTy_ = llvm::StructType::create(ctx_, {ptr, ptr, i64, i64}, "tz.jsonpath");
  auto *fnType = llvm::FunctionType::get(lower(type), {ptr, ptr}, false);
  llvm::StringRef name = type->name();
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.fromJson." + (name.starts_with("{") ? "object" : name),
                                    module_.get());
  setFunctionAttributes(fn);
  fromJsonFunctions_[type] = fn;

  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  llvm::Function *outer = function_;
  function_ = fn;
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *json = fn->getArg(0), *path = fn->getArg(1);
  llvm::Value *typeName = utf8Constant(name), *nameLength = builder_.getInt64(name.size());
  // Where the elements or the fields are, for their errors.
  llvm::Value *link = createStackSlot(jsonPathTy_, "path");
  builder_.CreateStore(path, builder_.CreateStructGEP(jsonPathTy_, link, 0));
  llvm::Value *keyObj = builder_.CreateStructGEP(jsonPathTy_, link, 1);
  llvm::Value *keyMeta = builder_.CreateStructGEP(jsonPathTy_, link, 2);
  llvm::Value *at = builder_.CreateStructGEP(jsonPathTy_, link, 3);
  // After each step: an error goes back to the caller, with `release` run.
  auto failIfThrown = [&](llvm::function_ref<void()> release) {
    llvm::BasicBlock *failBlock = newBlock("json.failed");
    llvm::BasicBlock *goBlock = newBlock("json.fits");
    builder_.CreateCondBr(builder_.CreateIsNotNull(builder_.CreateLoad(ptr, thrownSlot())),
                          failBlock, goBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(failBlock);
    release();
    builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
    switchTo(goBlock);
  };

  // Literal types: whether a Json holds the literal's value, and that value
  // as the program holds it (a string literal is its atom).
  llvm::Type *i32 = builder_.getInt32Ty(), *f64 = builder_.getDoubleTy();
  auto holdsLiteral = [&](const Type *literal, llvm::Value *j) {
    const Type *base = literal->literalBase();
    llvm::Value *is;
    if (base->isString()) {
      llvm::Value *text = stringLiteral(literal->literalText());
      is = builder_.CreateCall(jsonFunction("tz_json_is_string", i32, {ptr, ptr, i64}),
                               {j, stringObj(text), stringMeta(text)});
    } else if (base->isBool()) {
      is = builder_.CreateCall(jsonFunction("tz_json_is_bool", i32, {ptr, i32}),
                               {j, builder_.getInt32(literal->literalNumber() != 0)});
    } else {
      is = builder_.CreateCall(jsonFunction("tz_json_is_number", i32, {ptr, f64}),
                               {j, llvm::ConstantFP::get(f64, literal->literalNumber())});
    }
    return builder_.CreateICmpNE(is, builder_.getInt32(0));
  };
  auto literalValue = [&](const Type *literal) -> llvm::Value * {
    const Type *base = literal->literalBase();
    if (base->isString())
      return builder_.getInt32(literal->atom());
    if (base->isBool())
      return builder_.getInt1(literal->literalNumber() != 0);
    if (base->isFloat())
      return llvm::ConstantFP::get(f64, literal->literalNumber());
    return llvm::ConstantInt::getSigned(lower(base), static_cast<int64_t>(literal->literalNumber()));
  };
  // A TypeError for a value that is not what `what` names; the function
  // gives null then.
  auto mismatch = [&](llvm::Value *j, llvm::Value *at, llvm::StringRef what) {
    builder_.CreateCall(jsonFunction("tz_json_mismatch", builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
                        {j, at, utf8Constant(what), builder_.getInt64(what.size())});
    builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
  };

  if (type->isLiteral()) {
    llvm::BasicBlock *badBlock = newBlock("json.literal.bad");
    llvm::BasicBlock *goodBlock = newBlock("json.literal");
    builder_.CreateCondBr(holdsLiteral(type, json), goodBlock, badBlock);
    switchTo(badBlock);
    mismatch(json, path, name);
    switchTo(goodBlock);
    builder_.CreateRet(literalValue(type));
  } else if (type->isUnion()) {
    // The member a value goes to is told by what kind of value it is, then by
    // its value for a literal, and among objects by the tag field that Sema
    // found (see jsonTagField). The kinds are TZ_JSON_* of
    // runtime/core/tzrt/tzrt.h; tz_json_kind gives -1 for undefined.
    enum { kBool = 1, kNumber = 2, kString = 3, kArray = 4, kObject = 5 };
    std::map<int, std::vector<const Type *>> byKind;
    for (const Type *m : type->members()) {
      const Type *base = m->isLiteral() ? m->literalBase() : m;
      int kind = base->isBool()                           ? kBool
                 : base->isNumeric() || base->isDecimal() ? kNumber
                 : base->isString()                       ? kString
                 : base->isArray()                        ? kArray
                                                          : kObject;
      byKind[kind].push_back(m);
    }
    llvm::Value *kind = builder_.CreateCall(jsonFunction("tz_json_kind", i32, {ptr}), {json}, "kind");
    llvm::BasicBlock *badBlock = newBlock("json.union.bad");
    llvm::SwitchInst *sw = builder_.CreateSwitch(kind, badBlock, unsigned(byKind.size()));
    // Builds `member` from the Json found at `at`; its errors are its own.
    auto build = [&](const Type *member, llvm::Value *j) {
      llvm::Value *value = emitFromJson(member, j, path);
      failIfThrown([] {});
      builder_.CreateRet(emitConversion(value, member, type));
    };
    // Gives `literal` if `j` holds its value, and goes on otherwise.
    auto tryLiteral = [&](const Type *literal, llvm::Value *j, llvm::function_ref<void()> give) {
      llvm::BasicBlock *yesBlock = newBlock("json.union.literal");
      llvm::BasicBlock *noBlock = newBlock("json.union.next");
      builder_.CreateCondBr(holdsLiteral(literal, j), yesBlock, noBlock);
      switchTo(yesBlock);
      give();
      switchTo(noBlock);
    };
    for (const auto &[code, members] : byKind) {
      llvm::BasicBlock *block = newBlock("json.union.kind");
      sw->addCase(builder_.getInt32(code), block);
      switchTo(block);
      const Type *general = nullptr;
      std::vector<const Type *> objects;
      for (const Type *m : members) {
        if (m->isLiteral())
          tryLiteral(m, json, [&] { builder_.CreateRet(emitConversion(literalValue(m), m, type)); });
        else if (code == kObject)
          objects.push_back(m);
        else
          general = m;
      }
      if (objects.size() > 1) {
        // `{"kind": "circle", ...}`: the tag says which object it is.
        std::string tag = jsonTagField(objects);
        llvm::Value *key = stringLiteral(utf16(tag));
        builder_.CreateStore(stringObj(key), keyObj);
        builder_.CreateStore(stringMeta(key), keyMeta);
        builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
        llvm::Value *tagValue =
            builder_.CreateCall(jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                                {json, stringObj(key), stringMeta(key)}, tag);
        std::string tags;
        for (const Type *m : objects) {
          const Type *literal = m->field(tag)->type;
          tags += (tags.empty() ? "" : " | ") + literal->name().str();
          tryLiteral(literal, tagValue, [&] { build(m, json); });
        }
        mismatch(tagValue, link, tags);
        continue;
      }
      if (objects.size() == 1)
        general = objects.front();
      if (general)
        build(general, json);
      else
        mismatch(json, path, name);
    }
    switchTo(badBlock);
    mismatch(json, path, name);
  } else if (type->isArray()) {
    const Type *element = type->inner();
    llvm::Value *items = builder_.CreateCall(
        jsonFunction("tz_json_expect_array", ptr, {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength}, "items");
    failIfThrown([] {});
    llvm::Value *length = arrayLength(items);
    llvm::Value *result = newArray(Type::array(element), builder_.getInt64(0), length);
    builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), keyObj);
    builder_.CreateStore(builder_.getInt64(0), keyMeta);
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = newBlock("element");
    llvm::BasicBlock *bodyBlock = newBlock("element.body");
    llvm::BasicBlock *doneBlock = newBlock("elements.done");
    builder_.CreateBr(loopBlock);
    switchTo(loopBlock);
    llvm::PHINode *i = builder_.CreatePHI(i64, 2, "i");
    i->addIncoming(builder_.getInt64(0), entry);
    builder_.CreateCondBr(builder_.CreateICmpULT(i, length), bodyBlock, doneBlock);
    switchTo(bodyBlock);
    builder_.CreateStore(i, at);
    llvm::Value *item = builder_.CreateLoad(ptr, elementAddress(arrayData(items), Type::json(), i));
    llvm::Value *value = emitFromJson(element, item, link);
    failIfThrown([&] { releaseValue(result, type); });
    emitPush(result, {value}, element);
    i->addIncoming(builder_.CreateAdd(i, builder_.getInt64(1)), builder_.GetInsertBlock());
    builder_.CreateBr(loopBlock);
    switchTo(doneBlock);
    builder_.CreateRet(result);
  } else if (type->isRecord()) {
    // The keys in the order of the text, which the Record keeps.
    builder_.CreateCall(
        jsonFunction("tz_json_expect_object", builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfThrown([] {});
    const Type *value = type->valueType();
    llvm::Value *record = builder_.CreateCall(dictFunction("tz_map_new", ptr, {ptr}),
                                              {dictLayout(type).descriptor}, "record");
    llvm::Value *count =
        builder_.CreateCall(jsonFunction("tz_json_object_count", i32, {ptr}), {json}, "count");
    builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    llvm::Value *keySlot = createStackSlot(strTy_, "key");
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = newBlock("key");
    llvm::BasicBlock *bodyBlock = newBlock("key.body");
    llvm::BasicBlock *doneBlock = newBlock("keys.done");
    builder_.CreateBr(loopBlock);
    switchTo(loopBlock);
    llvm::PHINode *k = builder_.CreatePHI(i32, 2, "k");
    k->addIncoming(builder_.getInt32(0), entry);
    builder_.CreateCondBr(builder_.CreateICmpULT(k, count), bodyBlock, doneBlock);
    switchTo(bodyBlock);
    llvm::Value *item = builder_.CreateCall(
        jsonFunction("tz_json_object_entry", ptr, {ptr, i32, ptr}), {json, k, keySlot}, "item");
    llvm::Value *key = builder_.CreateLoad(strTy_, keySlot, "key"); // borrowed
    builder_.CreateStore(stringObj(key), keyObj);
    builder_.CreateStore(stringMeta(key), keyMeta);
    llvm::Value *converted = emitFromJson(value, item, link);
    failIfThrown([&] { releaseValue(record, type); });
    llvm::Value *slot = entryValue(emitDictAdd(record, key, Type::string()), type);
    storeIn(record, type, slot, converted, value);
    k->addIncoming(builder_.CreateAdd(k, builder_.getInt32(1)), builder_.GetInsertBlock());
    builder_.CreateBr(loopBlock);
    switchTo(doneBlock);
    builder_.CreateRet(record);
  } else {
    builder_.CreateCall(
        jsonFunction("tz_json_expect_object", builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfThrown([] {});
    // The keys in the order of the type, whatever the order in the JSON. The
    // values come first, so that an error leaves no object half made.
    llvm::ArrayRef<Field> fields = type->fields();
    builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    std::vector<llvm::Value *> values;
    for (unsigned k = 0; k < fields.size(); ++k) {
      llvm::Value *key = stringLiteral(utf16(fields[k].name));
      builder_.CreateStore(stringObj(key), keyObj);
      builder_.CreateStore(stringMeta(key), keyMeta);
      llvm::Value *item =
          builder_.CreateCall(jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                              {json, stringObj(key), stringMeta(key)}, fields[k].name);
      values.push_back(emitFromJson(fields[k].type, item, link));
      failIfThrown([&] {
        for (unsigned made = 0; made < k; ++made)
          releaseValue(values[made], fields[made].type);
      });
    }
    std::vector<unsigned> keys(fields.size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = allocObject(type, keys);
    const ObjectLayout &layout = objectLayout(type);
    for (unsigned k = 0; k < fields.size(); ++k) {
      builder_.CreateStore(values[k],
                           builder_.CreateStructGEP(layout.type, object, layout.elements[k]));
      if (isCyclic(type))
        groupStore(object, values[k], fields[k].type);
    }
    builder_.CreateRet(object);
  }
  function_ = outer;
  return fn;
}

} // namespace tz
