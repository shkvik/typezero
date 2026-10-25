#include "compiler/library/json/from_json_lowerer/from_json_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
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

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

} // namespace

FromJsonLowerer::FromJsonLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *FromJsonLowerer::emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  switch (type->kind()) {
  case Type::Kind::Int:
    return codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_expect_int", i32, {ptr, ptr}), {json, path});
  case Type::Kind::Long:
    return codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_expect_long", codegen_.builder_.getInt64Ty(), {ptr, ptr}), {json, path});
  case Type::Kind::Double:
    return codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_expect_double", codegen_.builder_.getDoubleTy(), {ptr, ptr}), {json, path});
  case Type::Kind::Bool:
    return codegen_.builder_.CreateTrunc(
        codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_expect_bool", i32, {ptr, ptr}), {json, path}),
        codegen_.builder_.getInt1Ty());
  case Type::Kind::String:
    return codegen_.stringLowerer().callReturningString(codegen_.jsonLowerer().jsonFunction("tz_json_expect_string", voidTy, {ptr, ptr, ptr}),
                               {json, path});
  case Type::Kind::Decimal: {
    llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.decTy_, "dec.out");
    codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_expect_decimal", voidTy, {ptr, ptr, ptr}),
                        {out, json, path});
    return codegen_.builder_.CreateLoad(codegen_.decTy_, out);
  }
  case Type::Kind::Json:
    return codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_expect_value", ptr, {ptr, ptr}), {json, path});
  case Type::Kind::Optional: {
    // A key that is not there gives undefined, and so does null, except for
    // a Json, which holds null.
    const Type *inner = type->inner();
    llvm::Value *present = inner->isJson() ? codegen_.builder_.CreateIsNotNull(json) : codegen_.jsonLowerer().jsonPresent(json);
    llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
    llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("json.value");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("json.optional");
    codegen_.builder_.CreateCondBr(present, valueBlock, endBlock);
    codegen_.functionFrame().switchTo(valueBlock);
    llvm::Value *value = codegen_.optionalLowerer().makeOptional(emitFromJson(inner, json, path), codegen_.builder_.getTrue(), type);
    llvm::BasicBlock *valueEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(type), 2);
    phi->addIncoming(codegen_.optionalLowerer().undefinedValue(type), before);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::Literal:
  case Type::Kind::Union:
    // An object of a class, through its constructor (fromJson(cls, j)).
    if (const ClassDecl *cls = type->classDecl(); cls && !cls->literal) {
      return codegen_.builder_.CreateCall(codegen_.reflectLowerer().classFromJsonFunction(cls), {json, path});
    }
    return codegen_.builder_.CreateCall(fromJsonFunction(type), {json, path});
  default:
    llvm_unreachable("Sema checked that JSON.parse<T> can build the type");
  }
}

// `T tz.fromJson.T(ptr json, ptr path)` for an array, an object, a literal
// type or a union. It is emitted aside, while the function that needs it is
// being emitted. A value that does not fit throws: the function releases
// what it built and gives null, which its caller drops when it sees
// tz_thrown.
llvm::Function *FromJsonLowerer::fromJsonFunction(const Type *type) {
  if (llvm::Function *fn = fromJsonFunctions_.lookup(type)) {
    return fn;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  if (!codegen_.jsonPathTy_) { // runtime/library/json/json/json.c: TzJsonPath
    codegen_.jsonPathTy_ = llvm::StructType::create(codegen_.ctx_, {ptr, ptr, i64, i64}, "tz.jsonpath");
  }
  auto *fnType = llvm::FunctionType::get(codegen_.moduleBuilder().lower(type), {ptr, ptr}, false);
  llvm::StringRef name = type->name();
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.fromJson." + (name.starts_with("{") ? "object" : name),
                                    codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(fn);
  fromJsonFunctions_[type] = fn;

  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  llvm::Function *outer = codegen_.function_;
  codegen_.function_ = fn;
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  llvm::Value *json = fn->getArg(0), *path = fn->getArg(1);
  llvm::Value *typeName = codegen_.stringLowerer().utf8Constant(name), *nameLength = codegen_.builder_.getInt64(name.size());
  // Where the elements or the fields are, for their errors.
  llvm::Value *link = codegen_.functionFrame().createStackSlot(codegen_.jsonPathTy_, "path");
  codegen_.builder_.CreateStore(path, codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 0));
  llvm::Value *keyObj = codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 1);
  llvm::Value *keyMeta = codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 2);
  llvm::Value *at = codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 3);
  // After each step: an error goes back to the caller, with `release` run.
  auto failIfThrown = [&](llvm::function_ref<void()> release) {
    llvm::BasicBlock *failBlock = codegen_.functionFrame().newBlock("json.failed");
    llvm::BasicBlock *goBlock = codegen_.functionFrame().newBlock("json.fits");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot())),
                          failBlock, goBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(failBlock);
    release();
    codegen_.builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
    codegen_.functionFrame().switchTo(goBlock);
  };

  // Literal types: whether a Json holds the literal's value, and that value
  // as the program holds it (a string literal is its atom).
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *f64 = codegen_.builder_.getDoubleTy();
  auto holdsLiteral = [&](const Type *literal, llvm::Value *j) {
    const Type *base = literal->literalBase();
    llvm::Value *is;
    if (base->isString()) {
      llvm::Value *text = codegen_.stringLowerer().stringLiteral(literal->literalText());
      is = codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_is_string", i32, {ptr, ptr, i64}),
                               {j, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
    } else if (base->isBool()) {
      is = codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_is_bool", i32, {ptr, i32}),
                               {j, codegen_.builder_.getInt32(literal->literalNumber() != 0)});
    } else {
      is = codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_is_number", i32, {ptr, f64}),
                               {j, llvm::ConstantFP::get(f64, literal->literalNumber())});
    }

    return codegen_.builder_.CreateICmpNE(is, codegen_.builder_.getInt32(0));
  };
  auto literalValue = [&](const Type *literal) -> llvm::Value * {
    const Type *base = literal->literalBase();
    if (base->isString()) {
      return codegen_.builder_.getInt32(literal->atom());
    }
    if (base->isBool()) {
      return codegen_.builder_.getInt1(literal->literalNumber() != 0);
    }
    if (base->isFloat()) {
      return llvm::ConstantFP::get(f64, literal->literalNumber());
    }

    return llvm::ConstantInt::getSigned(codegen_.moduleBuilder().lower(base),
                                        static_cast<int64_t>(literal->literalNumber()));
  };
  // A TypeError for a value that is not what `what` names; the function
  // gives null then.
  auto mismatch = [&](llvm::Value *j, llvm::Value *at, llvm::StringRef what) {
    codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_mismatch", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {j, at, codegen_.stringLowerer().utf8Constant(what), codegen_.builder_.getInt64(what.size())});
    codegen_.builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
  };

  if (type->isLiteral()) {
    llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("json.literal.bad");
    llvm::BasicBlock *goodBlock = codegen_.functionFrame().newBlock("json.literal");
    codegen_.builder_.CreateCondBr(holdsLiteral(type, json), goodBlock, badBlock);
    codegen_.functionFrame().switchTo(badBlock);
    mismatch(json, path, name);
    codegen_.functionFrame().switchTo(goodBlock);
    codegen_.builder_.CreateRet(literalValue(type));
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
    llvm::Value *kind =
        codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_kind", i32, {ptr}), {json}, "kind");
    llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("json.union.bad");
    llvm::SwitchInst *sw = codegen_.builder_.CreateSwitch(kind, badBlock, unsigned(byKind.size()));
    // Builds `member` from the Json found at `at`; its errors are its own.
    auto build = [&](const Type *member, llvm::Value *j) {
      llvm::Value *value = emitFromJson(member, j, path);
      failIfThrown([] {});
      codegen_.builder_.CreateRet(codegen_.exprLowerer().emitConversion(value, member, type));
    };
    // Gives `literal` if `j` holds its value, and goes on otherwise.
    auto tryLiteral = [&](const Type *literal, llvm::Value *j, llvm::function_ref<void()> give) {
      llvm::BasicBlock *yesBlock = codegen_.functionFrame().newBlock("json.union.literal");
      llvm::BasicBlock *noBlock = codegen_.functionFrame().newBlock("json.union.next");
      codegen_.builder_.CreateCondBr(holdsLiteral(literal, j), yesBlock, noBlock);
      codegen_.functionFrame().switchTo(yesBlock);
      give();
      codegen_.functionFrame().switchTo(noBlock);
    };
    for (const auto &[code, members] : byKind) {
      llvm::BasicBlock *block = codegen_.functionFrame().newBlock("json.union.kind");
      sw->addCase(codegen_.builder_.getInt32(code), block);
      codegen_.functionFrame().switchTo(block);
      const Type *general = nullptr;
      std::vector<const Type *> objects;
      for (const Type *m : members) {
        if (m->isLiteral()) {
          tryLiteral(m, json,
                     [&] { codegen_.builder_.CreateRet(codegen_.exprLowerer().emitConversion(literalValue(m), m, type)); });
        } else if (code == kObject) {
          objects.push_back(m);
        } else {
          general = m;
        }
      }
      if (objects.size() > 1) {
        // `{"kind": "circle", ...}`: the tag says which object it is.
        std::string tag = jsonTagField(objects);
        llvm::Value *key = codegen_.stringLowerer().stringLiteral(utf16(tag));
        codegen_.builder_.CreateStore(codegen_.stringLowerer().stringObj(key), keyObj);
        codegen_.builder_.CreateStore(codegen_.stringLowerer().stringMeta(key), keyMeta);
        codegen_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
        llvm::Value *tagValue =
            codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                                {json, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key)}, tag);
        std::string tags;
        for (const Type *m : objects) {
          const Type *literal = m->field(tag)->type;
          tags += (tags.empty() ? "" : " | ") + literal->name().str();
          tryLiteral(literal, tagValue, [&] { build(m, json); });
        }
        mismatch(tagValue, link, tags);
        continue;
      }
      if (objects.size() == 1) {
        general = objects.front();
      }
      if (general) {
        build(general, json);
      } else {
        mismatch(json, path, name);
      }
    }
    codegen_.functionFrame().switchTo(badBlock);
    mismatch(json, path, name);
  } else if (type->isArray()) {
    const Type *element = type->inner();
    llvm::Value *items =
        codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_expect_array", ptr, {ptr, ptr, ptr, i64}),
                            {json, path, typeName, nameLength}, "items");
    failIfThrown([] {});
    llvm::Value *length = codegen_.arrayLowerer().arrayLength(items);
    llvm::Value *result = codegen_.groupLowerer().newArray(Type::array(element), codegen_.builder_.getInt64(0), length);
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), keyObj);
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), keyMeta);
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = codegen_.functionFrame().newBlock("element");
    llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("element.body");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("elements.done");
    codegen_.builder_.CreateBr(loopBlock);
    codegen_.functionFrame().switchTo(loopBlock);
    llvm::PHINode *i = codegen_.builder_.CreatePHI(i64, 2, "i");
    i->addIncoming(codegen_.builder_.getInt64(0), entry);
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(i, length), bodyBlock, doneBlock);
    codegen_.functionFrame().switchTo(bodyBlock);
    codegen_.builder_.CreateStore(i, at);
    llvm::Value *item = codegen_.builder_.CreateLoad(ptr, codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(items), Type::json(), i));
    llvm::Value *value = emitFromJson(element, item, link);
    failIfThrown([&] { codegen_.refCounts().releaseValue(result, type); });
    codegen_.arrayLowerer().emitPush(result, {value}, element);
    i->addIncoming(codegen_.builder_.CreateAdd(i, codegen_.builder_.getInt64(1)), codegen_.builder_.GetInsertBlock());
    codegen_.builder_.CreateBr(loopBlock);
    codegen_.functionFrame().switchTo(doneBlock);
    codegen_.builder_.CreateRet(result);
  } else if (type->isRecord()) {
    // The keys in the order of the text, which the Record keeps.
    codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_expect_object", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfThrown([] {});
    const Type *value = type->valueType();
    llvm::Value *record = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr}),
                                              {codegen_.dictLowerer().dictLayout(type).descriptor}, "record");
    llvm::Value *count =
        codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_object_count", i32, {ptr}), {json}, "count");
    codegen_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    llvm::Value *keySlot = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "key");
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = codegen_.functionFrame().newBlock("key");
    llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("key.body");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("keys.done");
    codegen_.builder_.CreateBr(loopBlock);
    codegen_.functionFrame().switchTo(loopBlock);
    llvm::PHINode *k = codegen_.builder_.CreatePHI(i32, 2, "k");
    k->addIncoming(codegen_.builder_.getInt32(0), entry);
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(k, count), bodyBlock, doneBlock);
    codegen_.functionFrame().switchTo(bodyBlock);
    llvm::Value *item = codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_object_entry", ptr, {ptr, i32, ptr}), {json, k, keySlot}, "item");
    llvm::Value *key = codegen_.builder_.CreateLoad(codegen_.strTy_, keySlot, "key"); // borrowed
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringObj(key), keyObj);
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringMeta(key), keyMeta);
    llvm::Value *converted = emitFromJson(value, item, link);
    failIfThrown([&] { codegen_.refCounts().releaseValue(record, type); });
    llvm::Value *slot = codegen_.dictLowerer().entryValue(codegen_.dictLowerer().emitDictAdd(record, key, Type::string()), type);
    codegen_.storeLowerer().storeIn(record, type, slot, converted, value);
    k->addIncoming(codegen_.builder_.CreateAdd(k, codegen_.builder_.getInt32(1)), codegen_.builder_.GetInsertBlock());
    codegen_.builder_.CreateBr(loopBlock);
    codegen_.functionFrame().switchTo(doneBlock);
    codegen_.builder_.CreateRet(record);
  } else {
    codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_expect_object", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfThrown([] {});
    // The keys in the order of the type, whatever the order in the JSON. The
    // values come first, so that an error leaves no object half made.
    llvm::ArrayRef<Field> fields = type->fields();
    codegen_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    std::vector<llvm::Value *> values;
    for (unsigned k = 0; k < fields.size(); ++k) {
      llvm::Value *key = codegen_.stringLowerer().stringLiteral(utf16(fields[k].name));
      codegen_.builder_.CreateStore(codegen_.stringLowerer().stringObj(key), keyObj);
      codegen_.builder_.CreateStore(codegen_.stringLowerer().stringMeta(key), keyMeta);
      llvm::Value *item =
          codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                              {json, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key)}, fields[k].name);
      values.push_back(emitFromJson(fields[k].type, item, link));
      failIfThrown([&] {
        for (unsigned made = 0; made < k; ++made) {
          codegen_.refCounts().releaseValue(values[made], fields[made].type);
        }
      });
    }
    std::vector<unsigned> keys(fields.size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = codegen_.objectLayouts().allocObject(type, keys);
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
    for (unsigned k = 0; k < fields.size(); ++k) {
      codegen_.builder_.CreateStore(values[k],
                           codegen_.builder_.CreateStructGEP(layout.type, object, layout.elements[k]));
      if (codegen_.isCyclic(type)) {
        codegen_.storeLowerer().groupStore(object, values[k], fields[k].type);
      }
    }
    codegen_.builder_.CreateRet(object);
  }
  codegen_.function_ = outer;

  return fn;
}

} // namespace tz
