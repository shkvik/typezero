#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

CryptoLowerer::CryptoLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *CryptoLowerer::emitCryptoCall(CallExpr *call) {
  if (call->builtin == Builtin::CryptoKeyPair) {
    return emitCryptoKeyPair(call);
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy(), *voidTy = codegen_.builder_.getVoidTy();
  const CryptoFunction &fn = kCryptoFunctions[cast<IntLitExpr>(call->args[0])->value];
  std::vector<Expr *> exprs(call->args.begin() + 1, call->args.end());
  llvm::StringRef kinds = fn.params;
  assert(kinds.size() == exprs.size() && "the arguments are those of the function");

  // The values, owned: undefined data is not evaluated, and a hash or a cipher of the direct
  // layer is a constant of the runtime.
  std::vector<Expr *> evaluated;
  for (size_t i = 0; i < exprs.size(); ++i) {
    bool direct = (kinds[i] == 'm' || kinds[i] == 'n' || kinds[i] == 'h' || kinds[i] == 'c') &&
                  isa<IntLitExpr>(exprs[i]);
    if (!direct && !exprs[i]->type->isUndefined()) {
      evaluated.push_back(exprs[i]);
    }
  }
  std::vector<llvm::Value *> owned = codegen_.throwLowerer().emitHeldValues(evaluated);
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
    out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "crypto.text");
    params.push_back(ptr);
    args.push_back(out);
  }
  // A job of Web Crypto settles the promise it gets later (runtime/library/crypto/crypto_subtle*).
  llvm::Value *promise = nullptr;
  if (fn.result == 'P') {
    promise = codegen_.promiseLowerer().newPromise(call->type->promised());
    params.push_back(ptr);
    args.push_back(promise);
  }
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *v = values[i];
    const Type *type = exprs[i]->type;
    switch (kinds[i]) {
    case 'd':
      params.insert(params.end(), {ptr, ptr});
      if (!v) {
        args.insert(args.end(), {null, null});
      } else {
        args.insert(args.end(), {codegen_.typeDescriptors().typeDescriptor(type), codegen_.functionFrame().spill(v, "crypto.data")});
      }
      break;
    case 's':
      params.insert(params.end(), {ptr, i64});
      args.insert(args.end(), {codegen_.stringLowerer().stringObj(v), codegen_.stringLowerer().stringMeta(v)});
      break;
    case 'i':
      params.push_back(i32);
      args.push_back(v);
      break;
    case 'b':
      params.push_back(i32);
      args.push_back(codegen_.builder_.CreateZExt(v, i32));
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
        args.push_back(codegen_.module_->getOrInsertGlobal(alg.symbol, codegen_.builder_.getInt8Ty()));
      } else if (md) {
        int caller = kinds[i] == 'm' ? 0 : kinds[i] == 'n' ? 1 : 2;
        args.push_back(codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_openssl_md", ptr, {ptr, i64, i32}),
                                           {codegen_.stringLowerer().stringObj(v), codegen_.stringLowerer().stringMeta(v), codegen_.builder_.getInt32(caller)},
                                           "md"));
      } else {
        args.push_back(codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_openssl_cipher", ptr, {ptr, i64}),
                                           {codegen_.stringLowerer().stringObj(v), codegen_.stringLowerer().stringMeta(v)}, "cipher"));
      }
      break;
    }
    default:
      llvm_unreachable("an argument of an unknown kind");
    }
  }

  llvm::Type *ret = fn.result == 'o' || fn.result == 'p'   ? ptr
                    : fn.result == 'f' || fn.result == 't' ? f64
                    : fn.result == 'v' || fn.result == 's' || fn.result == 'S' || fn.result == 'P'
                        ? voidTy
                        : i32;
  llvm::Value *result = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction(fn.name, ret, params), args);
  if (promise) {
    // It rejects the promise rather than throwing.
    for (size_t i = 0; i < exprs.size(); ++i) {
      if (values[i]) {
        codegen_.refCounts().releaseValue(values[i], exprs[i]->type);
      }
    }
    return promise;
  }
  // What the call gives back of its receiver (or first argument) keeps its reference.
  llvm::Value *self = nullptr;
  if (fn.result == 'S') {
    self = values[0];
    if (exprs[0]->type->isManaged() && !isa<llvm::Constant>(self)) {
      values[0] = nullptr;
    }
  }
  for (size_t i = 0; i < exprs.size(); ++i) {
    if (values[i]) {
      codegen_.refCounts().releaseValue(values[i], exprs[i]->type);
    }
  }
  if (self && !values[0]) {
    codegen_.throwLowerer().emitThrowCheckWith([&] { codegen_.refCounts().releaseValue(self, exprs[0]->type); });
  } else {
    codegen_.throwLowerer().emitThrowCheck();
  }

  switch (fn.result) {
  case 'v':
    return nullptr;
  case 'S':
    return self;
  case 'o':
  case 'i':
  case 'f':
    return result;
  case 'p':
    return codegen_.optionalLowerer().makeOptional(result, codegen_.builder_.CreateIsNotNull(result), call->type);
  case 'b':
    return codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0));
  case 'j':
    return codegen_.optionalLowerer().makeOptional(result, codegen_.builder_.CreateICmpSGE(result, codegen_.builder_.getInt32(0)), call->type);
  case 's':
    return codegen_.builder_.CreateLoad(codegen_.strTy_, out);
  case 'q':
    return codegen_.optionalLowerer().makeOptional(codegen_.builder_.CreateLoad(codegen_.strTy_, out),
                        codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0)), call->type);
  case 't':
    return codegen_.dateLowerer().newDate(result);
  default:
    llvm_unreachable("a result of an unknown kind");
  }
}

// generateKeyPairSync: the private key, the public key of it, each exported as its encoding
// asks, in a new `{ publicKey, privateKey }`. The passphrases come first, as JavaScript reads
// the options before the call.
llvm::Value *CryptoLowerer::emitCryptoKeyPair(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  CodeGen::Held passphrases[2] = {{nullptr, nullptr, nullptr}, {nullptr, nullptr, nullptr}};
  for (int k = 0; k < 2; ++k) {
    if (Expr *p = call->args[1 + 5 * k + 4]; !p->type->isUndefined()) {
      passphrases[k] = codegen_.throwLowerer().hold(codegen_.exprLowerer().emitExpr(p), p->type, /*needed=*/true);
    }
  }
  llvm::Value *priv = codegen_.exprLowerer().emitExpr(call->args[0]);
  const Type *keyType = call->args[0]->type;
  llvm::Value *pub =
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_openssl_public_of", ptr, {ptr}), {priv}, "public");
  codegen_.throwLowerer().emitThrowCheckWith([&] { codegen_.refCounts().releaseValue(priv, keyType); });
  // The keys, then what each becomes; on the way up, what is not released yet.
  llvm::Value *values[2] = {pub, priv};
  const Type *types[2] = {keyType, keyType};
  for (int k = 0; k < 2; ++k) {
    Expr **part = &call->args[1 + 5 * k];
    int format = int32_t(cast<IntLitExpr>(part[0])->value);
    if (format < 0) {
      continue;
    }
    llvm::Value *has = codegen_.exprLowerer().emitExpr(part[1]);
    llvm::Value *keyTypeName = codegen_.exprLowerer().emitExpr(part[2]), *cipher = codegen_.exprLowerer().emitExpr(part[3]);
    llvm::Value *passType = null, *passValue = null;
    if (passphrases[k].type) {
      passType = codegen_.typeDescriptors().typeDescriptor(passphrases[k].type);
      passValue = codegen_.functionFrame().spill(codegen_.throwLowerer().take(passphrases[k]), "passphrase");
    }
    std::vector<llvm::Value *> rest = {has,
                                       codegen_.stringLowerer().stringObj(keyTypeName),
                                       codegen_.stringLowerer().stringMeta(keyTypeName),
                                       codegen_.stringLowerer().stringObj(cipher),
                                       codegen_.stringLowerer().stringMeta(cipher),
                                       passType,
                                       passValue};
    llvm::Value *key = values[k];
    if (format == 0) {
      llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "pem");
      std::vector<llvm::Value *> args = {out, key};
      args.insert(args.end(), rest.begin(), rest.end());
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_crypto_key_export_pem", codegen_.builder_.getVoidTy(),
                                          {ptr, ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                          args);
      values[k] = codegen_.builder_.CreateLoad(codegen_.strTy_, out);
    } else if (format == 1) {
      std::vector<llvm::Value *> args = {key};
      args.insert(args.end(), rest.begin(), rest.end());
      values[k] = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_crypto_key_export_der", ptr,
                                                      {ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                                      args);
    } else {
      values[k] =
          codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_crypto_key_export_jwk", ptr, {ptr}), {key});
    }
    types[k] = call->type->field(k == 0 ? "publicKey" : "privateKey")->type;
    codegen_.refCounts().releaseValue(keyTypeName, Type::string());
    codegen_.refCounts().releaseValue(cipher, Type::string());
    if (passphrases[k].type) {
      codegen_.refCounts().releaseValue(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(passphrases[k].type), passValue), passphrases[k].type);
    }
    codegen_.refCounts().releaseValue(key, keyType);
    codegen_.throwLowerer().emitThrowCheckWith([&] {
      for (int i = 0; i < 2; ++i) {
        codegen_.refCounts().releaseValue(values[i], types[i]);
      }
    });
  }
  for (CodeGen::Held &held : passphrases) {
    if (held.slot) {
      codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(held), held.type);
    }
  }
  const Type *type = call->type;
  llvm::Value *object = codegen_.objectLayouts().allocObject(type, {0, 1});
  codegen_.builder_.CreateStore(values[0], codegen_.objectLayouts().fieldAddress(object, type, "publicKey", /*exact=*/true));
  codegen_.builder_.CreateStore(values[1], codegen_.objectLayouts().fieldAddress(object, type, "privateKey", /*exact=*/true));

  return object;
}

} // namespace tz
