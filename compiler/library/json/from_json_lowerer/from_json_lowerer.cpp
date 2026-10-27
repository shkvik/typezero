#include "compiler/library/json/from_json_lowerer/from_json_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
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

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

}

FromJsonLowerer::FromJsonLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *FromJsonLowerer::emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  switch (type->kind()) {
  case Type::Kind::Int:
    return lowering_.builder_.CreateTrunc(
        lowering_.builder_.CreateCall(
            lowering_.jsonLowerer().jsonFunction("tz_json_expect_integer",
                                                 lowering_.builder_.getInt64Ty(), {ptr, ptr, i32}),
            {json, path, lowering_.builder_.getInt32(TypeDescriptors::descriptorKind(type))}),
        lowering_.moduleBuilder().lower(type));
  case Type::Kind::Double:
    return lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_double",
                                             lowering_.builder_.getDoubleTy(), {ptr, ptr}),
        {json, path});
  case Type::Kind::Bool:
    return lowering_.builder_.CreateTrunc(
        lowering_.builder_.CreateCall(
            lowering_.jsonLowerer().jsonFunction("tz_json_expect_bool", i32, {ptr, ptr}),
            {json, path}),
        lowering_.builder_.getInt1Ty());
  case Type::Kind::String:
    return lowering_.stringLowerer().callReturningString(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_string", voidTy, {ptr, ptr, ptr}),
        {json, path});
  case Type::Kind::Decimal: {
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.decTy_, "dec.out");
    lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_decimal", voidTy, {ptr, ptr, ptr}),
        {out, json, path});
    return lowering_.builder_.CreateLoad(lowering_.decTy_, out);
  }
  case Type::Kind::Json:
    return lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_value", ptr, {ptr, ptr}),
        {json, path});
  case Type::Kind::Optional: {
    const Type *inner = type->inner();
    llvm::Value *present = inner->isJson() ? lowering_.builder_.CreateIsNotNull(json)
                                           : lowering_.jsonLowerer().jsonPresent(json);
    llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
    llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("json.value");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("json.optional");
    lowering_.builder_.CreateCondBr(present, valueBlock, endBlock);
    lowering_.functionFrame().switchTo(valueBlock);
    llvm::Value *value = lowering_.optionalLowerer().makeOptional(
        emitFromJson(inner, json, path), lowering_.builder_.getTrue(), type);
    llvm::BasicBlock *valueEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(type), 2);
    phi->addIncoming(lowering_.optionalLowerer().undefinedValue(type), before);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::Literal:
  case Type::Kind::Union:
    if (const ClassDecl *cls = type->classDecl(); cls && !cls->literal) {
      return lowering_.builder_.CreateCall(lowering_.reflectLowerer().classFromJsonFunction(cls),
                                           {json, path});
    }
    return lowering_.builder_.CreateCall(fromJsonFunction(type), {json, path});
  default:
    llvm_unreachable("Checker checked that JSON.parse<T> can build the type");
  }
}

llvm::Value *FromJsonLowerer::emitFromJsonOrSkip(const Type *type, llvm::Value *json,
                                                 llvm::Value *path) {
  BasicBlock *convertBlock = lowering_.functionFrame().newBlock("json.convert");
  BasicBlock *skipBlock = lowering_.functionFrame().newBlock("json.skip");
  BasicBlock *endBlock = lowering_.functionFrame().newBlock("json.converted");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNull(json), skipBlock, convertBlock);
  lowering_.functionFrame().switchTo(convertBlock);
  Value *value = emitFromJson(type, json, path);
  BasicBlock *convertEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(skipBlock);
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(value->getType(), 2, "converted");
  phi->addIncoming(value, convertEnd);
  phi->addIncoming(llvm::Constant::getNullValue(value->getType()), skipBlock);

  return phi;
}

llvm::Function *FromJsonLowerer::fromJsonFunction(const Type *type) {
  if (llvm::Function *fn = fromJsonFunctions_.lookup(type)) {
    return fn;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  if (!lowering_.jsonPathTy_) {
    lowering_.jsonPathTy_ =
        llvm::StructType::create(lowering_.ctx_, {ptr, ptr, i64, i64}, "tz.jsonpath");
  }
  auto *fnType = llvm::FunctionType::get(lowering_.moduleBuilder().lower(type), {ptr, ptr}, false);
  llvm::StringRef name = type->name();
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.fromJson." + (name.starts_with("{") ? "object" : name),
                                    lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  fromJsonFunctions_[type] = fn;

  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  llvm::Function *outer = lowering_.function_;
  lowering_.function_ = fn;
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  llvm::Value *json = fn->getArg(0), *path = fn->getArg(1);
  llvm::Value *typeName = lowering_.stringLowerer().utf8Constant(name),
              *nameLength = lowering_.builder_.getInt64(name.size());
  llvm::Value *link = lowering_.functionFrame().createStackSlot(lowering_.jsonPathTy_, "path");
  lowering_.builder_.CreateStore(
      path, lowering_.builder_.CreateStructGEP(lowering_.jsonPathTy_, link, 0));
  llvm::Value *keyObj = lowering_.builder_.CreateStructGEP(lowering_.jsonPathTy_, link, 1);
  llvm::Value *keyMeta = lowering_.builder_.CreateStructGEP(lowering_.jsonPathTy_, link, 2);
  llvm::Value *at = lowering_.builder_.CreateStructGEP(lowering_.jsonPathTy_, link, 3);
  auto failIfFailed = [&](llvm::function_ref<void()> release) {
    llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("json.failed");
    llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("json.fits");
    lowering_.builder_.CreateCondBr(lowering_.resultLowerer().isFailed(), failBlock, goBlock,
                                    llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(failBlock);
    release();
    lowering_.builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
    lowering_.functionFrame().switchTo(goBlock);
  };

  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *f64 = lowering_.builder_.getDoubleTy();
  auto holdsLiteral = [&](const Type *literal, llvm::Value *j) {
    const Type *base = literal->literalBase();
    llvm::Value *is;
    if (base->isString()) {
      llvm::Value *text = lowering_.stringLowerer().stringLiteral(literal->literalText());
      is = lowering_.builder_.CreateCall(
          lowering_.jsonLowerer().jsonFunction("tz_json_is_string", i32, {ptr, ptr, i64}),
          {j, lowering_.stringLowerer().stringObj(text),
           lowering_.stringLowerer().stringMeta(text)});
    } else if (base->isBool()) {
      is = lowering_.builder_.CreateCall(
          lowering_.jsonLowerer().jsonFunction("tz_json_is_bool", i32, {ptr, i32}),
          {j, lowering_.builder_.getInt32(literal->literalNumber() != 0)});
    } else {
      is = lowering_.builder_.CreateCall(
          lowering_.jsonLowerer().jsonFunction("tz_json_is_number", i32, {ptr, f64}),
          {j, llvm::ConstantFP::get(f64, literal->literalNumber())});
    }

    return lowering_.builder_.CreateICmpNE(is, lowering_.builder_.getInt32(0));
  };
  auto literalValue = [&](const Type *literal) -> llvm::Value * {
    const Type *base = literal->literalBase();
    if (base->isString()) {
      return lowering_.builder_.getInt32(literal->atom());
    }
    if (base->isBool()) {
      return lowering_.builder_.getInt1(literal->literalNumber() != 0);
    }
    if (base->isFloat()) {
      return llvm::ConstantFP::get(f64, literal->literalNumber());
    }

    return llvm::ConstantInt::getSigned(lowering_.moduleBuilder().lower(base),
                                        static_cast<int64_t>(literal->literalNumber()));
  };
  auto mismatch = [&](llvm::Value *j, llvm::Value *at, llvm::StringRef what) {
    lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_mismatch", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr, ptr, i64}),
        {j, at, lowering_.stringLowerer().utf8Constant(what),
         lowering_.builder_.getInt64(what.size())});
    lowering_.builder_.CreateRet(llvm::Constant::getNullValue(fnType->getReturnType()));
  };

  if (type->isLiteral()) {
    llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("json.literal.bad");
    llvm::BasicBlock *goodBlock = lowering_.functionFrame().newBlock("json.literal");
    lowering_.builder_.CreateCondBr(holdsLiteral(type, json), goodBlock, badBlock);
    lowering_.functionFrame().switchTo(badBlock);
    mismatch(json, path, name);
    lowering_.functionFrame().switchTo(goodBlock);
    lowering_.builder_.CreateRet(literalValue(type));
  } else if (type->isUnion()) {
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
    llvm::Value *kind = lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_kind", i32, {ptr}), {json}, "kind");
    llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("json.union.bad");
    llvm::SwitchInst *sw = lowering_.builder_.CreateSwitch(kind, badBlock, unsigned(byKind.size()));
    auto build = [&](const Type *member, llvm::Value *j) {
      llvm::Value *value = emitFromJson(member, j, path);
      failIfFailed([] {});
      lowering_.builder_.CreateRet(lowering_.exprLowerer().emitConversion(value, member, type));
    };
    auto tryLiteral = [&](const Type *literal, llvm::Value *j, llvm::function_ref<void()> give) {
      llvm::BasicBlock *yesBlock = lowering_.functionFrame().newBlock("json.union.literal");
      llvm::BasicBlock *noBlock = lowering_.functionFrame().newBlock("json.union.next");
      lowering_.builder_.CreateCondBr(holdsLiteral(literal, j), yesBlock, noBlock);
      lowering_.functionFrame().switchTo(yesBlock);
      give();
      lowering_.functionFrame().switchTo(noBlock);
    };
    for (const auto &[code, members] : byKind) {
      llvm::BasicBlock *block = lowering_.functionFrame().newBlock("json.union.kind");
      sw->addCase(lowering_.builder_.getInt32(code), block);
      lowering_.functionFrame().switchTo(block);
      const Type *general = nullptr;
      std::vector<const Type *> objects;
      for (const Type *m : members) {
        if (m->isLiteral()) {
          tryLiteral(m, json, [&] {
            lowering_.builder_.CreateRet(
                lowering_.exprLowerer().emitConversion(literalValue(m), m, type));
          });
        } else if (code == kObject) {
          objects.push_back(m);
        } else {
          general = m;
        }
      }
      if (objects.size() > 1) {
        std::string tag = jsonTagField(objects);
        llvm::Value *key = lowering_.stringLowerer().stringLiteral(utf16(tag));
        lowering_.builder_.CreateStore(lowering_.stringLowerer().stringObj(key), keyObj);
        lowering_.builder_.CreateStore(lowering_.stringLowerer().stringMeta(key), keyMeta);
        lowering_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
        llvm::Value *tagValue = lowering_.builder_.CreateCall(
            lowering_.jsonLowerer().jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
            {json, lowering_.stringLowerer().stringObj(key),
             lowering_.stringLowerer().stringMeta(key)},
            tag);
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
    lowering_.functionFrame().switchTo(badBlock);
    mismatch(json, path, name);
  } else if (type->isArray()) {
    const Type *element = type->inner();
    llvm::Value *items = lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_array", ptr, {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength}, "items");
    failIfFailed([] {});
    llvm::Value *length = lowering_.arrayLowerer().arrayLength(items);
    llvm::Value *result = lowering_.groupLowerer().newArray(Type::array(element),
                                                            lowering_.builder_.getInt64(0), length);
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                   keyObj);
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), keyMeta);
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = lowering_.functionFrame().newBlock("element");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("element.body");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("elements.done");
    lowering_.builder_.CreateBr(loopBlock);
    lowering_.functionFrame().switchTo(loopBlock);
    llvm::PHINode *i = lowering_.builder_.CreatePHI(i64, 2, "i");
    i->addIncoming(lowering_.builder_.getInt64(0), entry);
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, length), bodyBlock,
                                    doneBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    lowering_.builder_.CreateStore(i, at);
    llvm::Value *item = lowering_.builder_.CreateLoad(
        ptr, lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(items),
                                                     Type::json(), i));
    llvm::Value *value = emitFromJson(element, item, link);
    failIfFailed([&] { lowering_.refCounts().releaseValue(result, type); });
    lowering_.arrayLowerer().emitPush(result, {value}, element);
    i->addIncoming(lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt64(1)),
                   lowering_.builder_.GetInsertBlock());
    lowering_.builder_.CreateBr(loopBlock);
    lowering_.functionFrame().switchTo(doneBlock);
    lowering_.builder_.CreateRet(result);
  } else if (type->isRecord()) {
    lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_object",
                                             lowering_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfFailed([] {});
    const Type *value = type->valueType();
    llvm::Value *record = lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr}),
        {lowering_.dictLowerer().dictLayout(type).descriptor}, "record");
    llvm::Value *count = lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_object_count", i32, {ptr}), {json}, "count");
    lowering_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    llvm::Value *keySlot = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "key");
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    llvm::BasicBlock *loopBlock = lowering_.functionFrame().newBlock("key");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("key.body");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("keys.done");
    lowering_.builder_.CreateBr(loopBlock);
    lowering_.functionFrame().switchTo(loopBlock);
    llvm::PHINode *k = lowering_.builder_.CreatePHI(i32, 2, "k");
    k->addIncoming(lowering_.builder_.getInt32(0), entry);
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(k, count), bodyBlock,
                                    doneBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    llvm::Value *item = lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_object_entry", ptr, {ptr, i32, ptr}),
        {json, k, keySlot}, "item");
    llvm::Value *key = lowering_.builder_.CreateLoad(lowering_.strTy_, keySlot, "key");
    lowering_.builder_.CreateStore(lowering_.stringLowerer().stringObj(key), keyObj);
    lowering_.builder_.CreateStore(lowering_.stringLowerer().stringMeta(key), keyMeta);
    llvm::Value *converted = emitFromJson(value, item, link);
    failIfFailed([&] { lowering_.refCounts().releaseValue(record, type); });
    llvm::Value *slot = lowering_.dictLowerer().entryValue(
        lowering_.dictLowerer().emitDictAdd(record, key, Type::string()), type);
    lowering_.storeLowerer().storeIn(record, type, slot, converted, value);
    k->addIncoming(lowering_.builder_.CreateAdd(k, lowering_.builder_.getInt32(1)),
                   lowering_.builder_.GetInsertBlock());
    lowering_.builder_.CreateBr(loopBlock);
    lowering_.functionFrame().switchTo(doneBlock);
    lowering_.builder_.CreateRet(record);
  } else {
    lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_expect_object",
                                             lowering_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
        {json, path, typeName, nameLength});
    failIfFailed([] {});
    llvm::ArrayRef<Field> fields = type->fields();
    lowering_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1), at);
    std::vector<llvm::Value *> values;
    for (unsigned k = 0; k < fields.size(); ++k) {
      llvm::Value *key = lowering_.stringLowerer().stringLiteral(utf16(fields[k].name));
      lowering_.builder_.CreateStore(lowering_.stringLowerer().stringObj(key), keyObj);
      lowering_.builder_.CreateStore(lowering_.stringLowerer().stringMeta(key), keyMeta);
      llvm::Value *item = lowering_.builder_.CreateCall(
          lowering_.jsonLowerer().jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
          {json, lowering_.stringLowerer().stringObj(key),
           lowering_.stringLowerer().stringMeta(key)},
          fields[k].name);
      values.push_back(emitFromJson(fields[k].type, item, link));
      failIfFailed([&] {
        for (unsigned made = 0; made < k; ++made) {
          lowering_.refCounts().releaseValue(values[made], fields[made].type);
        }
      });
    }
    std::vector<unsigned> keys(fields.size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, keys);
    const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
    for (unsigned k = 0; k < fields.size(); ++k) {
      lowering_.builder_.CreateStore(
          values[k], lowering_.builder_.CreateStructGEP(layout.type, object, layout.elements[k]));
      if (lowering_.isCyclic(type)) {
        lowering_.storeLowerer().groupStore(object, values[k], fields[k].type);
      }
    }
    lowering_.builder_.CreateRet(object);
  }
  lowering_.function_ = outer;

  return fn;
}

}
