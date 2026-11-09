#include "compiler/lower/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/result/err_sites/err_sites.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

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

}

CryptoLowerer::CryptoLowerer(Lowering &owner)
    : lowering(owner) {}

std::string CryptoLowerer::asciiOf(const Type *literal) {
  std::string text;
  for (char16_t unit : literal->literalText()) {
    text.push_back(char(unit));
  }

  return text;
}

std::vector<const Type *> CryptoLowerer::literalsOf(const Type *type) {
  if (type->isStringLiteral()) {
    return {type};
  }

  return std::vector<const Type *>(type->members().begin(), type->members().end());
}

llvm::StructType *CryptoLowerer::partsType() {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *str = this->lowering.strTy;

  return llvm::StructType::get(this->lowering.ctx,
                               {str, str, ptr, str, i64, i64, ptr, ptr, str,
                                this->lowering.builder.getDoubleTy(), str, ptr, str});
}

llvm::Function *CryptoLowerer::variantMaker(const Type *variant) {
  llvm::Function *&made = this->makers[variant];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make.crypto",
                                this->lowering.module_.get());
  llvm::Function *function = made;
  this->lowering.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function, function);
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::StructType *layout = this->partsType();
  auto load = [&](unsigned slot) {
    return this->lowering.builder.CreateLoad(
        layout->getElementType(slot), this->lowering.builder.CreateStructGEP(layout, parts, slot));
  };
  std::vector<unsigned> keys;
  for (const Field &field : variant->fields()) {
    keys.push_back(ObjectLayouts::fieldIndex(variant, field.name));
  }
  llvm::Value *object = this->lowering.objectLayouts().allocObject(variant, keys);
  for (const Field &field : variant->fields()) {
    llvm::StringRef name = field.name;
    const Type *t = field.type;
    bool atomic = UnionLayout::isAtomType(t);
    llvm::Value *value = nullptr;
    if (t->isStringLiteral()) {
      value = this->lowering.builder.getInt32(t->atom());
    } else if (name == "code") {
      value = this->lowering.objectMaker().atomOf(t, load(SlotCode));
    } else if (name == "message") {
      value = load(SlotMessage);
    } else if (name == "algorithm") {
      value = load(SlotAlgorithm);
    } else if (name == "length") {
      value = load(SlotLength);
    } else if (name == "expected") {
      value = atomic ? this->lowering.objectMaker().atomOf(t, load(SlotExpectedKind))
                     : load(SlotExpected);
    } else if (name == "got") {
      value = this->lowering.objectMaker().atomOf(t, load(SlotGot));
    } else if (name == "parameter") {
      value = load(SlotParameter);
    } else if (name == "value") {
      value = load(SlotValue);
    } else if (name == "field") {
      value = load(SlotField);
    } else if (name == "format") {
      value = this->lowering.objectMaker().atomOf(t, load(SlotFormat));
    } else if (name == "openssl") {
      value = load(SlotOpenssl);
    } else {
      continue;
    }
    this->lowering.builder.CreateStore(
        value, this->lowering.objectLayouts().fieldAddress(object, variant, name, true));
  }
  const std::pair<unsigned, llvm::StringRef> texts[] = {{SlotAlgorithm, "algorithm"},
                                                        {SlotParameter, "parameter"},
                                                        {SlotField, "field"},
                                                        {SlotOpenssl, "openssl"}};
  for (const auto &[slot, name] : texts) {
    if (!variant->field(name)) {
      this->lowering.refCounts().releaseValue(load(slot), Type::string());
    }
  }
  this->lowering.builder.CreateRet(object);
  this->lowering.function = outer;
  this->lowering.builder.restoreIP(point);

  return function;
}

llvm::Function *CryptoLowerer::errorMaker(const Type *error) {
  llvm::Function *&made = this->makers[error];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  std::vector<const Type *> variants =
      error->isUnion() ? std::vector<const Type *>(error->members().begin(), error->members().end())
                       : std::vector<const Type *>{error};
  std::vector<llvm::Function *> variantMakers;
  unsigned system = 0;
  bool unknownCode = false;
  for (size_t v = 0; v < variants.size(); ++v) {
    variantMakers.push_back(this->variantMaker(variants[v]));
    for (const Type *literal : CryptoLowerer::literalsOf(variants[v]->field("code")->type)) {
      if (CryptoLowerer::asciiOf(literal) == "UNKNOWN") {
        system = unsigned(v);
        unknownCode = true;
      }
    }
  }
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage,
                                "tz.make." + error->name().str(), this->lowering.module_.get());
  llvm::Function *function = made;
  this->lowering.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function, function);
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::StructType *layout = this->partsType();
  llvm::Value *codeAt = this->lowering.builder.CreateStructGEP(layout, parts, SlotCode);
  llvm::Value *code = this->lowering.builder.CreateLoad(ptr, codeAt, "code");
  this->lowering.refCounts().releaseValue(
      this->lowering.builder.CreateLoad(
          this->lowering.strTy,
          this->lowering.builder.CreateStructGEP(layout, parts, SlotCodeText)),
      Type::string());
  llvm::FunctionCallee compare = this->lowering.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  llvm::Value *index = llvm::ConstantInt::getSigned(i32, -1);
  for (size_t v = 0; v < variants.size(); ++v) {
    for (const Type *literal : CryptoLowerer::literalsOf(variants[v]->field("code")->type)) {
      llvm::Value *same = this->lowering.builder.CreateICmpEQ(
          this->lowering.builder.CreateCall(
              compare,
              {code, this->lowering.stringLowerer().utf8Constant(CryptoLowerer::asciiOf(literal))}),
          this->lowering.builder.getInt32(0));
      index = this->lowering.builder.CreateSelect(
          same, this->lowering.builder.getInt32(unsigned(v)), index);
    }
  }
  llvm::BasicBlock *unknown = llvm::BasicBlock::Create(this->lowering.ctx, "unknown", function);
  llvm::SwitchInst *pick =
      this->lowering.builder.CreateSwitch(index, unknown, unsigned(variants.size()));
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::BasicBlock *block = llvm::BasicBlock::Create(this->lowering.ctx, "variant", function);
    pick->addCase(this->lowering.builder.getInt32(unsigned(v)), block);
    this->lowering.builder.SetInsertPoint(block);
    this->lowering.builder.CreateRet(this->lowering.builder.CreateCall(variantMakers[v], {parts}));
  }
  this->lowering.builder.SetInsertPoint(unknown);
  if (unknownCode) {
    this->lowering.builder.CreateStore(this->lowering.stringLowerer().utf8Constant("UNKNOWN"),
                                       codeAt);
  }
  this->lowering.builder.CreateRet(
      this->lowering.builder.CreateCall(variantMakers[system], {parts}));
  this->lowering.function = outer;
  this->lowering.builder.restoreIP(point);

  return function;
}

void CryptoLowerer::useErrors(const Type *errorType) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *maker = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  if (errorType && CryptoErrorTypes::isCryptoError(errorType)) {
    maker = this->errorMaker(errorType);
  } else if (errorType) {
    maker = this->lowering.resultLowerer().failureMaker(errorType);
  }
  this->lowering.builder.CreateCall(
      this->lowering.processLowerer().processFunction("tz_crypto_errors",
                                                      this->lowering.builder.getVoidTy(), {ptr}),
      {maker});
}

llvm::Value *CryptoLowerer::emitCryptoCall(CallExpr *call) {
  if (call->builtin() == Builtin::CryptoKeyPair) {
    return this->emitCryptoKeyPair(call);
  }
  if (call->builtin() == Builtin::CryptoKeyPairAsync) {
    return this->emitCryptoKeyPairAsync(call);
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy(), *i32 = this->lowering.builder.getInt32Ty(),
             *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
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
  std::vector<llvm::Value *> owned = this->lowering.exprLowerer().emitExprs(evaluated);
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
    out = this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "crypto.text");
    params.push_back(ptr);
    args.push_back(out);
  }
  llvm::Value *promise = nullptr;
  const Type *settled = fn.result == 'P' ? call->type->promised() : nullptr;
  bool settlesResult = settled && settled->isResult();
  if (fn.result == 'P') {
    promise = this->lowering.promiseLowerer().newPromise(settled);
    params.push_back(ptr);
    args.push_back(promise);
  }
  if (fallible) {
    this->useErrors(call->type->errType());
  }
  if (settlesResult) {
    this->useErrors(settled->errType());
  }
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *v = values[i];
    const Type *type = exprs[i]->type;
    switch (kinds[i]) {
    case 'd':
      params.insert(params.end(), {ptr, ptr});
      if (!v) {
        args.insert(args.end(), {null, null});
      } else {
        args.insert(args.end(), {this->lowering.typeDescriptors().typeDescriptor(type),
                                 this->lowering.functionFrame().spill(v, "crypto.data")});
      }
      break;
    case 's':
      params.insert(params.end(), {ptr, i64});
      args.insert(args.end(), {this->lowering.stringLowerer().stringObj(v),
                               this->lowering.stringLowerer().stringMeta(v)});
      break;
    case 'i':
      params.push_back(i32);
      args.push_back(v->getType() == i32 ? v : this->lowering.builder.CreateTrunc(v, i32));
      break;
    case 'b':
      params.push_back(i32);
      args.push_back(this->lowering.builder.CreateZExt(v, i32));
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
        args.push_back(this->lowering.module_->getOrInsertGlobal(
            alg.symbol, this->lowering.builder.getInt8Ty()));
      } else if (md) {
        int caller = kinds[i] == 'm' ? 0 : kinds[i] == 'n' ? 1 : 2;
        args.push_back(this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_openssl_md", ptr, {ptr, i64, i32}),
            {this->lowering.stringLowerer().stringObj(v),
             this->lowering.stringLowerer().stringMeta(v), this->lowering.builder.getInt32(caller)},
            "md"));
      } else {
        args.push_back(this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_openssl_cipher", ptr, {ptr, i64}),
            {this->lowering.stringLowerer().stringObj(v),
             this->lowering.stringLowerer().stringMeta(v)},
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
                    : fn.result == 't'                   ? this->lowering.builder.getInt64Ty()
                    : fn.result == 'v' || fn.result == 's' || fn.result == 'S' || fn.result == 'P'
                        ? voidTy
                        : i32;
  llvm::Value *result = this->lowering.builder.CreateCall(
      this->lowering.processLowerer().processFunction(fn.name, ret, params), args);
  if (fallible || settlesResult) {
    this->useErrors(nullptr);
  }
  if (promise) {
    for (size_t i = 0; i < exprs.size(); ++i) {
      if (values[i]) {
        this->lowering.refCounts().releaseValue(values[i], exprs[i]->type);
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
      this->lowering.refCounts().releaseValue(values[i], exprs[i]->type);
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
      return this->lowering.builder.CreateSExt(result, this->lowering.builder.getInt64Ty());
    case 'o':
    case 'f':
      return result;
    case 'p':
      return this->lowering.optionalLowerer().makeOptional(
          result, this->lowering.builder.CreateIsNotNull(result), valueType);
    case 'b':
      return this->lowering.builder.CreateICmpNE(result, this->lowering.builder.getInt32(0));
    case 'j':
      return this->lowering.optionalLowerer().makeOptional(
          this->lowering.builder.CreateSExt(result, this->lowering.builder.getInt64Ty()),
          this->lowering.builder.CreateICmpSGE(result, this->lowering.builder.getInt32(0)),
          valueType);
    case 's':
      return this->lowering.builder.CreateLoad(this->lowering.strTy, out);
    case 'q':
      return this->lowering.optionalLowerer().makeOptional(
          this->lowering.builder.CreateLoad(this->lowering.strTy, out),
          this->lowering.builder.CreateICmpNE(result, this->lowering.builder.getInt32(0)),
          valueType);
    case 't':
      return this->lowering.dateLowerer().newDate(result);
    default:
      llvm_unreachable("a result of an unknown kind");
    }
  };
  if (!fallible) {
    return plain();
  }

  return this->lowering.resultLowerer().catchFailure(
      call->type, call->loc(), [&]() -> llvm::Value * {
        llvm::Value *value = plain();
        if (!textEncoding) {
          return value;
        }
        const Type *bytes = Type::bytesType(Type::Bytes::Buffer);
        llvm::Value *text = this->lowering.stringLowerer().callReturningString(
            this->lowering.processLowerer().processFunction("tz_buffer_to_string", voidTy,
                                                            {ptr, ptr, i32, i64, i64, i32}),
            {value, this->lowering.builder.getInt32(int32_t(textEncoding->value)),
             this->lowering.builder.getInt64(0), this->lowering.builder.getInt64(0),
             this->lowering.builder.getInt32(0)});
        this->lowering.refCounts().releaseValue(value, bytes);

        return text;
      });
}

llvm::Value *CryptoLowerer::emitCryptoKeyPair(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy(), *i32 = this->lowering.builder.getInt32Ty(),
             *i64 = this->lowering.builder.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());

  const Type *passTypes[2] = {nullptr, nullptr};
  llvm::Value *passSlots[2] = {nullptr, nullptr};
  for (int k = 0; k < 2; ++k) {
    if (Expr *p = call->args[1 + 5 * k + 4]; !p->type->isUndefined()) {
      passTypes[k] = p->type;
      passSlots[k] = this->lowering.functionFrame().createManagedSlot(p->type, "passphrase.held");
      this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(p), passSlots[k]);
    }
  }
  auto takePassphrase = [&](int k) {
    llvm::Type *lowered = this->lowering.moduleBuilder().lower(passTypes[k]);
    llvm::Value *value = this->lowering.builder.CreateLoad(lowered, passSlots[k]);
    this->lowering.builder.CreateStore(llvm::Constant::getNullValue(lowered), passSlots[k]);

    return value;
  };
  const Type *resultType = call->type;
  const Type *pairType = resultType->okType();
  llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("keypair.failed");
  bool failing = false;
  auto failWith = [&](llvm::function_ref<void()> release) {
    llvm::Value *failure = this->lowering.builder.CreateLoad(
        ptr, this->lowering.resultLowerer().failedSlot(), "failure");
    llvm::BasicBlock *bad = this->lowering.functionFrame().newBlock("keypair.bad");
    llvm::BasicBlock *good = this->lowering.functionFrame().newBlock("keypair.good");
    this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(failure), bad, good);
    this->lowering.functionFrame().switchTo(bad);
    for (int k = 0; k < 2; ++k) {
      if (passSlots[k]) {
        this->lowering.refCounts().releaseValue(takePassphrase(k), passTypes[k]);
      }
    }
    release();
    this->lowering.builder.CreateBr(failBlock);
    failing = true;
    this->lowering.functionFrame().switchTo(good);
  };
  this->useErrors(resultType->errType());
  llvm::Value *priv = this->lowering.exprLowerer().emitExpr(call->args[0]);
  const Type *keyType = call->args[0]->type;
  failWith([] {});
  llvm::Value *pub = this->lowering.builder.CreateCall(
      this->lowering.processLowerer().processFunction("tz_openssl_public_of", ptr, {ptr}), {priv},
      "public");
  failWith([&] { this->lowering.refCounts().releaseValue(priv, keyType); });
  llvm::Value *values[2] = {pub, priv};
  const Type *types[2] = {keyType, keyType};
  for (int k = 0; k < 2; ++k) {
    Expr **part = &call->args[1 + 5 * k];
    int format = int32_t(cast<IntLitExpr>(part[0])->value);
    if (format < 0) {
      continue;
    }
    llvm::Value *has = this->lowering.builder.CreateTrunc(
        this->lowering.exprLowerer().emitExpr(part[1]), this->lowering.builder.getInt32Ty());
    llvm::Value *keyTypeName = this->lowering.exprLowerer().emitExpr(part[2]),
                *cipher = this->lowering.exprLowerer().emitExpr(part[3]);
    llvm::Value *passType = null;
    llvm::Value *passValue = null;
    if (passTypes[k]) {
      passType = this->lowering.typeDescriptors().typeDescriptor(passTypes[k]);
      passValue = this->lowering.functionFrame().spill(takePassphrase(k), "passphrase");
      passSlots[k] = nullptr;
    }
    std::vector<llvm::Value *> rest = {has,
                                       this->lowering.stringLowerer().stringObj(keyTypeName),
                                       this->lowering.stringLowerer().stringMeta(keyTypeName),
                                       this->lowering.stringLowerer().stringObj(cipher),
                                       this->lowering.stringLowerer().stringMeta(cipher),
                                       passType,
                                       passValue};
    llvm::Value *key = values[k];
    if (format == 0) {
      llvm::AllocaInst *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "pem");
      std::vector<llvm::Value *> args = {out, key};
      args.insert(args.end(), rest.begin(), rest.end());
      this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
                                            "tz_crypto_key_export_pem",
                                            this->lowering.builder.getVoidTy(),
                                            {ptr, ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                                        args);
      values[k] = this->lowering.builder.CreateLoad(this->lowering.strTy, out);
    } else if (format == 1) {
      std::vector<llvm::Value *> args = {key};
      args.insert(args.end(), rest.begin(), rest.end());
      values[k] = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_crypto_key_export_der", ptr,
                                                          {ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
          args);
    } else {
      values[k] = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_crypto_key_export_jwk", ptr, {ptr}),
          {key});
    }
    types[k] = pairType->field(k == 0 ? "publicKey" : "privateKey")->type;
    this->lowering.refCounts().releaseValue(keyTypeName, Type::string());
    this->lowering.refCounts().releaseValue(cipher, Type::string());
    if (passTypes[k]) {
      this->lowering.refCounts().releaseValue(
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(passTypes[k]),
                                            passValue),
          passTypes[k]);
    }
    this->lowering.refCounts().releaseValue(key, keyType);
    failWith([&] {
      for (int i = 0; i < 2; ++i) {
        this->lowering.refCounts().releaseValue(values[i], types[i]);
      }
    });
  }
  for (int k = 0; k < 2; ++k) {
    if (passSlots[k]) {
      this->lowering.refCounts().releaseValue(takePassphrase(k), passTypes[k]);
    }
  }
  this->useErrors(nullptr);
  const Type *type = pairType;
  llvm::Value *object = this->lowering.objectLayouts().allocObject(type, {0, 1});
  this->lowering.builder.CreateStore(
      values[0], this->lowering.objectLayouts().fieldAddress(object, type, "publicKey", true));
  this->lowering.builder.CreateStore(
      values[1], this->lowering.objectLayouts().fieldAddress(object, type, "privateKey", true));
  llvm::Value *made = this->lowering.resultLowerer().makeResult(
      resultType, this->lowering.builder.getTrue(), object, nullptr);
  llvm::BasicBlock *madeEnd = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("keypair.end");
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(failBlock);
  llvm::PHINode *phi = nullptr;
  llvm::Value *failed = nullptr;
  llvm::BasicBlock *failEnd = nullptr;
  if (failing) {
    this->useErrors(nullptr);
    llvm::Value *slot = this->lowering.resultLowerer().failedSlot();
    llvm::Value *failure = this->lowering.builder.CreateLoad(ptr, slot, "failure");
    this->lowering.builder.CreateStore(
        llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy()), slot);
    failed = this->lowering.resultLowerer().makeResult(
        resultType, this->lowering.builder.getFalse(), nullptr, failure,
        this->lowering.errSites().site(call->loc()));
    failEnd = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateBr(endBlock);
  } else {
    this->lowering.builder.CreateUnreachable();
  }
  this->lowering.functionFrame().switchTo(endBlock);
  phi = this->lowering.builder.CreatePHI(made->getType(), failing ? 2 : 1, "keypair");
  phi->addIncoming(made, madeEnd);
  if (failing) {
    phi->addIncoming(failed, failEnd);
  }

  return phi;
}

llvm::Function *CryptoLowerer::pairMaker(const Type *pair) {
  llvm::Function *&made = this->makers[pair];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make.keypair",
                                this->lowering.module_.get());
  llvm::Function *function = made;
  this->lowering.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function, function);
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", function));
  llvm::StructType *layout = llvm::StructType::get(
      this->lowering.ctx, {this->lowering.strTy, this->lowering.strTy, ptr, ptr});
  llvm::Value *values = function->getArg(0);
  llvm::Value *object = this->lowering.objectLayouts().allocObject(pair, {0, 1});
  const char *names[2] = {"publicKey", "privateKey"};
  for (unsigned k = 0; k < 2; ++k) {
    bool text = pair->field(names[k])->type->isString();
    llvm::Value *value = this->lowering.builder.CreateLoad(
        text ? static_cast<llvm::Type *>(this->lowering.strTy) : ptr,
        this->lowering.builder.CreateStructGEP(layout, values, text ? k : 2 + k));
    this->lowering.builder.CreateStore(
        value, this->lowering.objectLayouts().fieldAddress(object, pair, names[k], true));
  }
  this->lowering.builder.CreateRet(object);
  this->lowering.function = outer;
  this->lowering.builder.restoreIP(point);

  return function;
}

llvm::Value *CryptoLowerer::emitCryptoKeyPairAsync(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy(), *i32 = this->lowering.builder.getInt32Ty(),
             *i64 = this->lowering.builder.getInt64Ty(),
             *f64 = this->lowering.builder.getDoubleTy();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  auto *generate = cast<CallExpr>(call->args[0]);
  auto int32Of = [&](Expr *e) {
    return this->lowering.builder.getInt32(uint32_t(cast<IntLitExpr>(e)->value));
  };
  llvm::Value *name = this->lowering.exprLowerer().emitExpr(generate->args[2]);
  llvm::Value *modulus = this->lowering.exprLowerer().emitExpr(generate->args[3]);
  llvm::Value *exponent = this->lowering.exprLowerer().emitExpr(generate->args[4]);
  llvm::Value *curve = this->lowering.exprLowerer().emitExpr(generate->args[6]);
  std::vector<llvm::Type *> params = {ptr, ptr, i32, ptr, i64, f64, f64, i32, ptr, i64};
  std::vector<llvm::Value *> args;
  const Type *settled = call->type->promised();
  llvm::Value *promise = this->lowering.promiseLowerer().newPromise(settled);
  args.insert(args.end(),
              {promise, this->pairMaker(settled->okType()), int32Of(generate->args[1]),
               this->lowering.stringLowerer().stringObj(name),
               this->lowering.stringLowerer().stringMeta(name), modulus, exponent,
               int32Of(generate->args[5]), this->lowering.stringLowerer().stringObj(curve),
               this->lowering.stringLowerer().stringMeta(curve)});
  std::vector<std::pair<llvm::Value *, const Type *>> owned = {{name, Type::string()},
                                                               {curve, Type::string()}};
  for (int k = 0; k < 2; ++k) {
    Expr **part = &call->args[1 + 5 * k];
    llvm::Value *flags =
        this->lowering.builder.CreateTrunc(this->lowering.exprLowerer().emitExpr(part[1]), i32);
    llvm::Value *keyType = this->lowering.exprLowerer().emitExpr(part[2]);
    llvm::Value *cipher = this->lowering.exprLowerer().emitExpr(part[3]);
    llvm::Value *passType = null;
    llvm::Value *passValue = null;
    if (!part[4]->type->isUndefined()) {
      llvm::Value *pass = this->lowering.exprLowerer().emitExpr(part[4]);
      passType = this->lowering.typeDescriptors().typeDescriptor(part[4]->type);
      passValue = this->lowering.functionFrame().spill(pass, "passphrase");
      owned.push_back({pass, part[4]->type});
    }
    params.insert(params.end(), {i32, i32, ptr, i64, ptr, i64, ptr, ptr});
    args.insert(args.end(),
                {int32Of(part[0]), flags, this->lowering.stringLowerer().stringObj(keyType),
                 this->lowering.stringLowerer().stringMeta(keyType),
                 this->lowering.stringLowerer().stringObj(cipher),
                 this->lowering.stringLowerer().stringMeta(cipher), passType, passValue});
    owned.push_back({keyType, Type::string()});
    owned.push_back({cipher, Type::string()});
  }
  this->useErrors(settled->errType());
  this->lowering.builder.CreateCall(
      this->lowering.processLowerer().processFunction("tz_openssl_generate_key_pair_async",
                                                      this->lowering.builder.getVoidTy(), params),
      args);
  this->useErrors(nullptr);
  for (const auto &[value, type] : owned) {
    this->lowering.refCounts().releaseValue(value, type);
  }

  return promise;
}

}
