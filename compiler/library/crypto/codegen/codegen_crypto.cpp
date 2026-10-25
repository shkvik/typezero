// node:crypto (docs/crypto.md): calls of the functions of runtime/crypto*.c that Sema chose
// (sema_crypto.cpp), each argument passed as its kind in kCryptoFunctions says, and the result
// made of what the function gives (see sema/crypto_api.h). The runtime borrows the arguments;
// every call may throw.

#include "codegen/codegen.h"
#include "sema/crypto_api.h"

#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

llvm::Value *CodeGen::emitCryptoCall(CallExpr *call) {
  if (call->builtin == Builtin::CryptoKeyPair)
    return emitCryptoKeyPair(call);
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy(), *voidTy = builder_.getVoidTy();
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
    if (!direct && !exprs[i]->type->isUndefined())
      evaluated.push_back(exprs[i]);
  }
  std::vector<llvm::Value *> owned = emitHeldValues(evaluated);
  std::vector<llvm::Value *> values(exprs.size(), nullptr);
  for (size_t i = 0, k = 0; i < exprs.size(); ++i)
    if (k < evaluated.size() && evaluated[k] == exprs[i])
      values[i] = owned[k++];

  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  llvm::AllocaInst *out = nullptr;
  if (fn.result == 's' || fn.result == 'q') {
    out = createStackSlot(strTy_, "crypto.text");
    params.push_back(ptr);
    args.push_back(out);
  }
  // A job of Web Crypto settles the promise it gets later (runtime/crypto_subtle*.c).
  llvm::Value *promise = nullptr;
  if (fn.result == 'P') {
    promise = newPromise(call->type->promised());
    params.push_back(ptr);
    args.push_back(promise);
  }
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *v = values[i];
    const Type *type = exprs[i]->type;
    switch (kinds[i]) {
    case 'd':
      params.insert(params.end(), {ptr, ptr});
      if (!v)
        args.insert(args.end(), {null, null});
      else
        args.insert(args.end(), {typeDescriptor(type), spill(v, "crypto.data")});
      break;
    case 's':
      params.insert(params.end(), {ptr, i64});
      args.insert(args.end(), {stringObj(v), stringMeta(v)});
      break;
    case 'i':
      params.push_back(i32);
      args.push_back(v);
      break;
    case 'b':
      params.push_back(i32);
      args.push_back(builder_.CreateZExt(v, i32));
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
        args.push_back(module_->getOrInsertGlobal(alg.symbol, builder_.getInt8Ty()));
      } else if (md) {
        int caller = kinds[i] == 'm' ? 0 : kinds[i] == 'n' ? 1 : 2;
        args.push_back(builder_.CreateCall(processFunction("tz_openssl_md", ptr, {ptr, i64, i32}),
                                           {stringObj(v), stringMeta(v), builder_.getInt32(caller)},
                                           "md"));
      } else {
        args.push_back(builder_.CreateCall(processFunction("tz_openssl_cipher", ptr, {ptr, i64}),
                                           {stringObj(v), stringMeta(v)}, "cipher"));
      }
      break;
    }
    default:
      llvm_unreachable("an argument of an unknown kind");
    }
  }

  llvm::Type *ret = fn.result == 'o' || fn.result == 'p' ? ptr
                    : fn.result == 'f' || fn.result == 't' ? f64
                    : fn.result == 'v' || fn.result == 's' || fn.result == 'S' || fn.result == 'P'
                        ? voidTy
                        : i32;
  llvm::Value *result = builder_.CreateCall(processFunction(fn.name, ret, params), args);
  if (promise) {
    // It rejects the promise rather than throwing.
    for (size_t i = 0; i < exprs.size(); ++i)
      if (values[i])
        releaseValue(values[i], exprs[i]->type);
    return promise;
  }
  // What the call gives back of its receiver (or first argument) keeps its reference.
  llvm::Value *self = nullptr;
  if (fn.result == 'S') {
    self = values[0];
    if (exprs[0]->type->isManaged() && !isa<llvm::Constant>(self))
      values[0] = nullptr;
  }
  for (size_t i = 0; i < exprs.size(); ++i)
    if (values[i])
      releaseValue(values[i], exprs[i]->type);
  if (self && !values[0])
    emitThrowCheckWith([&] { releaseValue(self, exprs[0]->type); });
  else
    emitThrowCheck();

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
    return makeOptional(result, builder_.CreateIsNotNull(result), call->type);
  case 'b':
    return builder_.CreateICmpNE(result, builder_.getInt32(0));
  case 'j':
    return makeOptional(result, builder_.CreateICmpSGE(result, builder_.getInt32(0)), call->type);
  case 's':
    return builder_.CreateLoad(strTy_, out);
  case 'q':
    return makeOptional(builder_.CreateLoad(strTy_, out),
                        builder_.CreateICmpNE(result, builder_.getInt32(0)), call->type);
  case 't':
    return newDate(result);
  default:
    llvm_unreachable("a result of an unknown kind");
  }
}

// generateKeyPairSync: the private key, the public key of it, each exported as its encoding
// asks, in a new `{ publicKey, privateKey }`. The passphrases come first, as JavaScript reads
// the options before the call.
llvm::Value *CodeGen::emitCryptoKeyPair(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  Held passphrases[2] = {{nullptr, nullptr, nullptr}, {nullptr, nullptr, nullptr}};
  for (int k = 0; k < 2; ++k)
    if (Expr *p = call->args[1 + 5 * k + 4]; !p->type->isUndefined())
      passphrases[k] = hold(emitExpr(p), p->type, /*needed=*/true);
  llvm::Value *priv = emitExpr(call->args[0]);
  const Type *keyType = call->args[0]->type;
  llvm::Value *pub =
      builder_.CreateCall(processFunction("tz_openssl_public_of", ptr, {ptr}), {priv}, "public");
  emitThrowCheckWith([&] { releaseValue(priv, keyType); });
  // The keys, then what each becomes; on the way up, what is not released yet.
  llvm::Value *values[2] = {pub, priv};
  const Type *types[2] = {keyType, keyType};
  for (int k = 0; k < 2; ++k) {
    Expr **part = &call->args[1 + 5 * k];
    int format = int32_t(cast<IntLitExpr>(part[0])->value);
    if (format < 0)
      continue;
    llvm::Value *has = emitExpr(part[1]);
    llvm::Value *keyTypeName = emitExpr(part[2]), *cipher = emitExpr(part[3]);
    llvm::Value *passType = null, *passValue = null;
    if (passphrases[k].type) {
      passType = typeDescriptor(passphrases[k].type);
      passValue = spill(take(passphrases[k]), "passphrase");
    }
    std::vector<llvm::Value *> rest = {has,
                                       stringObj(keyTypeName),
                                       stringMeta(keyTypeName),
                                       stringObj(cipher),
                                       stringMeta(cipher),
                                       passType,
                                       passValue};
    llvm::Value *key = values[k];
    if (format == 0) {
      llvm::AllocaInst *out = createStackSlot(strTy_, "pem");
      std::vector<llvm::Value *> args = {out, key};
      args.insert(args.end(), rest.begin(), rest.end());
      builder_.CreateCall(processFunction("tz_crypto_key_export_pem", builder_.getVoidTy(),
                                          {ptr, ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                          args);
      values[k] = builder_.CreateLoad(strTy_, out);
    } else if (format == 1) {
      std::vector<llvm::Value *> args = {key};
      args.insert(args.end(), rest.begin(), rest.end());
      values[k] = builder_.CreateCall(processFunction("tz_crypto_key_export_der", ptr,
                                                      {ptr, i32, ptr, i64, ptr, i64, ptr, ptr}),
                                      args);
    } else {
      values[k] =
          builder_.CreateCall(processFunction("tz_crypto_key_export_jwk", ptr, {ptr}), {key});
    }
    types[k] = call->type->field(k == 0 ? "publicKey" : "privateKey")->type;
    releaseValue(keyTypeName, Type::string());
    releaseValue(cipher, Type::string());
    if (passphrases[k].type)
      releaseValue(builder_.CreateLoad(lower(passphrases[k].type), passValue), passphrases[k].type);
    releaseValue(key, keyType);
    emitThrowCheckWith([&] {
      for (int i = 0; i < 2; ++i)
        releaseValue(values[i], types[i]);
    });
  }
  for (Held &held : passphrases)
    if (held.slot)
      releaseValue(take(held), held.type);
  const Type *type = call->type;
  llvm::Value *object = allocObject(type, {0, 1});
  builder_.CreateStore(values[0], fieldAddress(object, type, "publicKey", /*exact=*/true));
  builder_.CreateStore(values[1], fieldAddress(object, type, "privateKey", /*exact=*/true));
  return object;
}

} // namespace tz
