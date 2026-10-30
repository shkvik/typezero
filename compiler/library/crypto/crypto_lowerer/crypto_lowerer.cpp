#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum PartsSlot : unsigned {
  SlotCodeText,
  SlotMessage,
  SlotCode,
  SlotAlgorithm,
  SlotLength,
  SlotExpected,
  SlotGot,
  SlotExpectedKind,
  SlotParameter,
  SlotValue,
  SlotField,
  SlotFormat,
  SlotOpenssl,
};

std::string asciiOf(const Type *literal) {
  std::string text;
  for (char16_t unit : literal->literalText()) {
    text.push_back(char(unit));
  }

  return text;
}

std::vector<const Type *> literalsOf(const Type *type) {
  if (type->isStringLiteral()) {
    return {type};
  }

  return std::vector<const Type *>(type->members().begin(), type->members().end());
}

}

CryptoLowerer::CryptoLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *CryptoLowerer::partsType() {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *str = lowering_.strTy_;

  return llvm::StructType::get(lowering_.ctx_, {str, str, ptr, str, i64, i64, ptr, ptr, str,
                                                lowering_.builder_.getDoubleTy(), str, ptr, str});
}

llvm::Function *CryptoLowerer::variantMaker(const Type *variant) {
  llvm::Function *&made = makers_[variant];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make.crypto",
                                lowering_.module_.get());
  llvm::Function *function = made;
  lowering_.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, function);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::StructType *layout = partsType();
  auto load = [&](unsigned slot) {
    return lowering_.builder_.CreateLoad(layout->getElementType(slot),
                                         lowering_.builder_.CreateStructGEP(layout, parts, slot));
  };
  std::vector<unsigned> keys;
  for (const Field &field : variant->fields()) {
    keys.push_back(ObjectLayouts::fieldIndex(variant, field.name));
  }
  llvm::Value *object = lowering_.objectLayouts().allocObject(variant, keys);
  for (const Field &field : variant->fields()) {
    llvm::StringRef name = field.name;
    const Type *t = field.type;
    bool atomic = UnionLayout::isAtomType(t);
    llvm::Value *value = nullptr;
    if (t->isStringLiteral()) {
      value = lowering_.builder_.getInt32(t->atom());
    } else if (name == "code") {
      value = lowering_.objectMaker().atomOf(t, load(SlotCode));
    } else if (name == "message") {
      value = load(SlotMessage);
    } else if (name == "algorithm") {
      value = load(SlotAlgorithm);
    } else if (name == "length") {
      value = load(SlotLength);
    } else if (name == "expected") {
      value =
          atomic ? lowering_.objectMaker().atomOf(t, load(SlotExpectedKind)) : load(SlotExpected);
    } else if (name == "got") {
      value = lowering_.objectMaker().atomOf(t, load(SlotGot));
    } else if (name == "parameter") {
      value = load(SlotParameter);
    } else if (name == "value") {
      value = load(SlotValue);
    } else if (name == "field") {
      value = load(SlotField);
    } else if (name == "format") {
      value = lowering_.objectMaker().atomOf(t, load(SlotFormat));
    } else if (name == "openssl") {
      value = load(SlotOpenssl);
    } else {
      continue;
    }
    lowering_.builder_.CreateStore(
        value, lowering_.objectLayouts().fieldAddress(object, variant, name, true));
  }
  const std::pair<unsigned, llvm::StringRef> texts[] = {{SlotAlgorithm, "algorithm"},
                                                        {SlotParameter, "parameter"},
                                                        {SlotField, "field"},
                                                        {SlotOpenssl, "openssl"}};
  for (const auto &[slot, name] : texts) {
    if (!variant->field(name)) {
      lowering_.refCounts().releaseValue(load(slot), Type::string());
    }
  }
  lowering_.builder_.CreateRet(object);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return function;
}

llvm::Function *CryptoLowerer::errorMaker(const Type *error) {
  llvm::Function *&made = makers_[error];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  std::vector<const Type *> variants =
      error->isUnion() ? std::vector<const Type *>(error->members().begin(), error->members().end())
                       : std::vector<const Type *>{error};
  std::vector<llvm::Function *> makers;
  unsigned system = 0;
  bool unknownCode = false;
  for (size_t v = 0; v < variants.size(); ++v) {
    makers.push_back(variantMaker(variants[v]));
    for (const Type *literal : literalsOf(variants[v]->field("code")->type)) {
      if (asciiOf(literal) == "UNKNOWN") {
        system = unsigned(v);
        unknownCode = true;
      }
    }
  }
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage,
                                "tz.make." + error->name().str(), lowering_.module_.get());
  llvm::Function *function = made;
  lowering_.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, function);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::StructType *layout = partsType();
  llvm::Value *codeAt = lowering_.builder_.CreateStructGEP(layout, parts, SlotCode);
  llvm::Value *code = lowering_.builder_.CreateLoad(ptr, codeAt, "code");
  lowering_.refCounts().releaseValue(
      lowering_.builder_.CreateLoad(
          lowering_.strTy_, lowering_.builder_.CreateStructGEP(layout, parts, SlotCodeText)),
      Type::string());
  llvm::FunctionCallee compare = lowering_.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  llvm::Value *index = llvm::ConstantInt::getSigned(i32, -1);
  for (size_t v = 0; v < variants.size(); ++v) {
    for (const Type *literal : literalsOf(variants[v]->field("code")->type)) {
      llvm::Value *same = lowering_.builder_.CreateICmpEQ(
          lowering_.builder_.CreateCall(
              compare, {code, lowering_.stringLowerer().utf8Constant(asciiOf(literal))}),
          lowering_.builder_.getInt32(0));
      index =
          lowering_.builder_.CreateSelect(same, lowering_.builder_.getInt32(unsigned(v)), index);
    }
  }
  llvm::BasicBlock *unknown = llvm::BasicBlock::Create(lowering_.ctx_, "unknown", function);
  llvm::SwitchInst *pick =
      lowering_.builder_.CreateSwitch(index, unknown, unsigned(variants.size()));
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::BasicBlock *block = llvm::BasicBlock::Create(lowering_.ctx_, "variant", function);
    pick->addCase(lowering_.builder_.getInt32(unsigned(v)), block);
    lowering_.builder_.SetInsertPoint(block);
    lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[v], {parts}));
  }
  lowering_.builder_.SetInsertPoint(unknown);
  if (unknownCode) {
    lowering_.builder_.CreateStore(lowering_.stringLowerer().utf8Constant("UNKNOWN"), codeAt);
  }
  lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[system], {parts}));
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return function;
}

void CryptoLowerer::useErrors(const Type *errorType) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *maker = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (errorType && CryptoErrorTypes::isCryptoError(errorType)) {
    maker = errorMaker(errorType);
  } else if (errorType) {
    maker = lowering_.resultLowerer().failureMaker(errorType);
  }
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_crypto_errors", lowering_.builder_.getVoidTy(), {ptr}),
                                {maker});
}

llvm::Value *CryptoLowerer::emitCryptoCall(CallExpr *call) {
  if (call->builtin == Builtin::CryptoKeyPair) {
    return emitCryptoKeyPair(call);
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy(), *voidTy = lowering_.builder_.getVoidTy();
  const CryptoFunction &fn = kCryptoFunctions[cast<IntLitExpr>(call->args[0])->value];
  std::vector<Expr *> exprs(call->args.begin() + 1, call->args.end());
  llvm::StringRef kinds = fn.params;
  bool fallible = call->type->isResult();
  const Type *valueType = fallible ? call->type->okType() : call->type;
  IntLitExpr *textEncoding = nullptr;
  if (exprs.size() == kinds.size() + 1) {
    textEncoding = cast<IntLitExpr>(exprs.back());
    exprs.pop_back();
  }
  assert(kinds.size() == exprs.size() && "the arguments are those of the function");

  std::vector<Expr *> evaluated;
  for (size_t i = 0; i < exprs.size(); ++i) {
    bool direct = (kinds[i] == 'm' || kinds[i] == 'n' || kinds[i] == 'h' || kinds[i] == 'c') &&
                  isa<IntLitExpr>(exprs[i]);
    if (!direct && !exprs[i]->type->isUndefined()) {
      evaluated.push_back(exprs[i]);
    }
  }
  std::vector<llvm::Value *> owned = lowering_.exprLowerer().emitExprs(evaluated);
  std::vector<llvm::Value *> values(exprs.size(), nullptr);
  for (size_t i = 0, k = 0; i < exprs.size(); ++i) {
    if (k < evaluated.size() && evaluated[k] == exprs[i]) {
      values[i] = owned[k++];
    }
  }

  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  llvm::AllocaInst *out = nullptr;
  if (fn.result == 's' || fn.result == 'q') {
    out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "crypto.text");
    params.push_back(ptr);
    args.push_back(out);
  }
  llvm::Value *promise = nullptr;
  if (fn.result == 'P') {
    promise = lowering_.promiseLowerer().newPromise(call->type->promised());
    params.push_back(ptr);
    args.push_back(promise);
  }
  if (fallible) {
    useErrors(call->type->errType());
  }
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *v = values[i];
    const Type *type = exprs[i]->type;
    switch (kinds[i]) {
    case 'd':
      params.insert(params.end(), {ptr, ptr});
      if (!v) {
        args.insert(args.end(), {null, null});
      } else {
        args.insert(args.end(), {lowering_.typeDescriptors().typeDescriptor(type),
                                 lowering_.functionFrame().spill(v, "crypto.data")});
      }
      break;
    case 's':
      params.insert(params.end(), {ptr, i64});
      args.insert(args.end(), {lowering_.stringLowerer().stringObj(v),
                               lowering_.stringLowerer().stringMeta(v)});
      break;
    case 'i':
      params.push_back(i32);
      args.push_back(v->getType() == i32 ? v : lowering_.builder_.CreateTrunc(v, i32));
      break;
    case 'b':
      params.push_back(i32);
      args.push_back(lowering_.builder_.CreateZExt(v, i32));
      break;
    case 'f':
      params.push_back(f64);
      args.push_back(v);
      break;
    case 'o':
      params.push_back(ptr);
      args.push_back(v);
      break;
    case 'm':
    case 'n':
    case 'h':
    case 'c': {
      params.push_back(ptr);
      bool md = kinds[i] != 'c';
      if (auto *lit = dyn_cast<IntLitExpr>(exprs[i])) {
        const DirectAlgorithm &alg = md ? kDirectMds[lit->value] : kDirectCiphers[lit->value];
        args.push_back(
            lowering_.module_->getOrInsertGlobal(alg.symbol, lowering_.builder_.getInt8Ty()));
      } else if (md) {
        int caller = kinds[i] == 'm' ? 0 : kinds[i] == 'n' ? 1 : 2;
        args.push_back(lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_openssl_md", ptr, {ptr, i64, i32}),
            {lowering_.stringLowerer().stringObj(v), lowering_.stringLowerer().stringMeta(v),
             lowering_.builder_.getInt32(caller)},
            "md"));
      } else {
        args.push_back(lowering_.builder_.CreateCall(
            lowering_.processLowerer().processFunction("tz_openssl_cipher", ptr, {ptr, i64}),
            {lowering_.stringLowerer().stringObj(v), lowering_.stringLowerer().stringMeta(v)},
            "cipher"));
      }
      break;
    }
    default:
      llvm_unreachable("an argument of an unknown kind");
    }
  }

  llvm::Type *ret = fn.result == 'o' || fn.result == 'p' ? ptr
                    : fn.result == 'f'                   ? f64
                    : fn.result == 't'                   ? lowering_.builder_.getInt64Ty()
                    : fn.result == 'v' || fn.result == 's' || fn.result == 'S' || fn.result == 'P'
                        ? voidTy
                        : i32;
  llvm::Value *result = lowering_.builder_.CreateCall(
      lowering_.processLowerer().processFunction(fn.name, ret, params), args);
  if (fallible) {
    useErrors(nullptr);
  }
  if (promise) {
    for (size_t i = 0; i < exprs.size(); ++i) {
      if (values[i]) {
        lowering_.refCounts().releaseValue(values[i], exprs[i]->type);
      }
    }
    return promise;
  }
  llvm::Value *self = nullptr;
  if (fn.result == 'S' && !fallible) {
    self = values[0];
    if (exprs[0]->type->isManaged() && !isa<llvm::Constant>(self)) {
      values[0] = nullptr;
    }
  }
  for (size_t i = 0; i < exprs.size(); ++i) {
    if (values[i]) {
      lowering_.refCounts().releaseValue(values[i], exprs[i]->type);
    }
  }
  if (self && !values[0]) {
  } else {
  }

  auto plain = [&]() -> llvm::Value * {
    switch (fn.result) {
    case 'v':
      return nullptr;
    case 'S':
      return self;
    case 'i':
      return lowering_.builder_.CreateSExt(result, lowering_.builder_.getInt64Ty());
    case 'o':
    case 'f':
      return result;
    case 'p':
      return lowering_.optionalLowerer().makeOptional(
          result, lowering_.builder_.CreateIsNotNull(result), valueType);
    case 'b':
      return lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0));
    case 'j':
      return lowering_.optionalLowerer().makeOptional(
          lowering_.builder_.CreateSExt(result, lowering_.builder_.getInt64Ty()),
          lowering_.builder_.CreateICmpSGE(result, lowering_.builder_.getInt32(0)), valueType);
    case 's':
      return lowering_.builder_.CreateLoad(lowering_.strTy_, out);
    case 'q':
      return lowering_.optionalLowerer().makeOptional(
          lowering_.builder_.CreateLoad(lowering_.strTy_, out),
          lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0)), valueType);
    case 't':
      return lowering_.dateLowerer().newDate(result);
    default:
      llvm_unreachable("a result of an unknown kind");
    }
  };
  if (!fallible) {
    return plain();
  }

  return lowering_.resultLowerer().catchFailure(call->type, call->loc(), [&]() -> llvm::Value * {
    llvm::Value *value = plain();
    if (!textEncoding) {
      return value;
    }
    const Type *bytes = Type::bytesType(Type::Bytes::Buffer);
    llvm::Value *text = lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction("tz_buffer_to_string", voidTy,
                                                   {ptr, ptr, i32, i64, i64, i32}),
        {value, lowering_.builder_.getInt32(int32_t(textEncoding->value)),
         lowering_.builder_.getInt64(0), lowering_.builder_.getInt64(0),
         lowering_.builder_.getInt32(0)});
    lowering_.refCounts().releaseValue(value, bytes);

    return text;
  });
}

llvm::Value *CryptoLowerer::emitCryptoKeyPair(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());

  const Type *passTypes[2] = {nullptr, nullptr};
  llvm::Value *passSlots[2] = {nullptr, nullptr};
  for (int k = 0; k < 2; ++k) {
    if (Expr *p = call->args[1 + 5 * k + 4]; !p->type->isUndefined()) {
      passTypes[k] = p->type;
      passSlots[k] = lowering_.functionFrame().createManagedSlot(p->type, "passphrase.held");
      lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(p), passSlots[k]);
    }
  }
  auto takePassphrase = [&](int k) {
    llvm::Type *lowered = lowering_.moduleBuilder().lower(passTypes[k]);
    llvm::Value *value = lowering_.builder_.CreateLoad(lowered, passSlots[k]);
    lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), passSlots[k]);

    return value;
  };
  const Type *resultType = call->type;
  const Type *pairType = resultType->okType();
  llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("keypair.failed");
  bool failing = false;
  auto failWith = [&](llvm::function_ref<void()> release) {
    llvm::Value *failure =
        lowering_.builder_.CreateLoad(ptr, lowering_.resultLowerer().failedSlot(), "failure");
    llvm::BasicBlock *bad = lowering_.functionFrame().newBlock("keypair.bad");
    llvm::BasicBlock *good = lowering_.functionFrame().newBlock("keypair.good");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(failure), bad, good);
    lowering_.functionFrame().switchTo(bad);
    for (int k = 0; k < 2; ++k) {
      if (passSlots[k]) {
        lowering_.refCounts().releaseValue(takePassphrase(k), passTypes[k]);
      }
    }
    release();
    lowering_.builder_.CreateBr(failBlock);
    failing = true;
    lowering_.functionFrame().switchTo(good);
  };
  useErrors(resultType->errType());
  llvm::Value *priv = lowering_.exprLowerer().emitExpr(call->args[0]);
  const Type *keyType = call->args[0]->type;
  failWith([] {});
  llvm::Value *pub = lowering_.builder_.CreateCall(
      lowering_.processLowerer().processFunction("tz_openssl_public_of", ptr, {ptr}), {priv},
      "public");
  failWith([&] { lowering_.refCounts().releaseValue(priv, keyType); });
  llvm::Value *values[2] = {pub, priv};
  const Type *types[2] = {keyType, keyType};
  for (int k = 0; k < 2; ++k) {
    Expr **part = &call->args[1 + 5 * k];
    int format = int32_t(cast<IntLitExpr>(part[0])->value);
    if (format < 0) {
      continue;
    }
    llvm::Value *has = lowering_.builder_.CreateTrunc(lowering_.exprLowerer().emitExpr(part[1]),
                                                      lowering_.builder_.getInt32Ty());
    llvm::Value *keyTypeName = lowering_.exprLowerer().emitExpr(part[2]),
                *cipher = lowering_.exprLowerer().emitExpr(part[3]);
    llvm::Value *passType = null, *passValue = null;
    if (passTypes[k]) {
      passType = lowering_.typeDescriptors().typeDescriptor(passTypes[k]);
      passValue = lowering_.functionFrame().spill(takePassphrase(k), "passphrase");
      passSlots[k] = nullptr;
    }
    std::vector<llvm::Value *> rest = {has,
                                       lowering_.stringLowerer().stringObj(keyTypeName),
                                       lowering_.stringLowerer().stringMeta(keyTypeName),
                                       lowering_.stringLowerer().stringObj(cipher),
                                       lowering_.stringLowerer().stringMeta(cipher),
                                       passType,
                                       passValue};
    llvm::Value *key = values[k];
    if (format == 0) {
      llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "pem");
      std::vector<llvm::Value *> args = {out, key};
      args.insert(args.end(), rest.begin(), rest.end());
      lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                        "tz_crypto_key_export_pem", lowering_.builder_.getVoidTy(),
                                        {ptr, ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                                    args);
      values[k] = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
    } else if (format == 1) {
      std::vector<llvm::Value *> args = {key};
      args.insert(args.end(), rest.begin(), rest.end());
      values[k] = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_crypto_key_export_der", ptr,
                                                     {ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
          args);
    } else {
      values[k] = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_crypto_key_export_jwk", ptr, {ptr}),
          {key});
    }
    types[k] = pairType->field(k == 0 ? "publicKey" : "privateKey")->type;
    lowering_.refCounts().releaseValue(keyTypeName, Type::string());
    lowering_.refCounts().releaseValue(cipher, Type::string());
    if (passTypes[k]) {
      lowering_.refCounts().releaseValue(
          lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(passTypes[k]), passValue),
          passTypes[k]);
    }
    lowering_.refCounts().releaseValue(key, keyType);
    failWith([&] {
      for (int i = 0; i < 2; ++i) {
        lowering_.refCounts().releaseValue(values[i], types[i]);
      }
    });
  }
  for (int k = 0; k < 2; ++k) {
    if (passSlots[k]) {
      lowering_.refCounts().releaseValue(takePassphrase(k), passTypes[k]);
    }
  }
  useErrors(nullptr);
  const Type *type = pairType;
  llvm::Value *object = lowering_.objectLayouts().allocObject(type, {0, 1});
  lowering_.builder_.CreateStore(
      values[0], lowering_.objectLayouts().fieldAddress(object, type, "publicKey", true));
  lowering_.builder_.CreateStore(
      values[1], lowering_.objectLayouts().fieldAddress(object, type, "privateKey", true));
  llvm::Value *made = lowering_.resultLowerer().makeResult(resultType, lowering_.builder_.getTrue(),
                                                           object, nullptr);
  llvm::BasicBlock *madeEnd = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("keypair.end");
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(failBlock);
  llvm::PHINode *phi = nullptr;
  llvm::Value *failed = nullptr;
  llvm::BasicBlock *failEnd = nullptr;
  if (failing) {
    useErrors(nullptr);
    llvm::Value *slot = lowering_.resultLowerer().failedSlot();
    llvm::Value *failure = lowering_.builder_.CreateLoad(ptr, slot, "failure");
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                   slot);
    failed =
        lowering_.resultLowerer().makeResult(resultType, lowering_.builder_.getFalse(), nullptr,
                                             failure, lowering_.errSites().site(call->loc()));
    failEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
  } else {
    lowering_.builder_.CreateUnreachable();
  }
  lowering_.functionFrame().switchTo(endBlock);
  phi = lowering_.builder_.CreatePHI(made->getType(), failing ? 2 : 1, "keypair");
  phi->addIncoming(made, madeEnd);
  if (failing) {
    phi->addIncoming(failed, failEnd);
  }

  return phi;
}

}
