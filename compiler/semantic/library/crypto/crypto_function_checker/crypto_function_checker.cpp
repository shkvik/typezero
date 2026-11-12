#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args_errors.h"
#include "compiler/semantic/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker_errors.h"
#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports.h"
#include "compiler/semantic/library/crypto/crypto_names/crypto_names.h"
#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CryptoFunctionChecker::CryptoFunctionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->cryptoArgs = &all.cryptoArgs;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->bytesChecker = &all.bytesChecker;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->keyPairChecker = &all.keyPairChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::optional<std::string> CryptoFunctionChecker::literalOf(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }

  return std::nullopt;
}

Expr *CryptoFunctionChecker::checkCryptoCall(CallExpr *call, llvm::StringRef functionName,
                                             llvm::SMLoc loc) {
  llvm::StringRef sync = functionName;
  bool isAsync = false;
  for (const auto &[form, syncName] : kAsyncForms) {
    if (functionName == form) {
      isAsync = true;
      sync = syncName;
    }
  }
  for (const auto &[form, syncName] : kTzCryptoAsyncForms) {
    if (functionName == form) {
      isAsync = true;
      sync = syncName;
    }
  }
  if (isAsync && !call->args.empty() && isa<FunctionExpr>(call->args.back())) {
    auto message = ErrorTextCryptoFunction::callback(functionName);
    this->state.getDiagnostics().error(call->args.back()->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  auto *checked = this->checkSyncCall(call, sync, functionName, loc);

  return isAsync ? this->asyncOf(checked) : checked;
}

CryptoFunctionChecker::ResultFilledTarget
CryptoFunctionChecker::filledTarget(const Type *type, llvm::StringRef functionName,
                                    llvm::StringRef shown) const {
  if (functionName == "getRandomValues") {
    static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                         Type::Bytes::Float64};
    if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
      auto message = ErrorTextCryptoFunction::randomVals(type->name());

      return err(CryptoFunctionCheckerFilledTargetError{
          .code = CryptoFunctionCheckerFilledTargetErrorCode::NOT_INTEGERS,
          .message = message,
      });
    }

    return ok(type);
  }
  if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
    auto message = ErrorTextCryptoFunction::fillsBytes(shown, type->name());

    return err(CryptoFunctionCheckerFilledTargetError{
        .code = CryptoFunctionCheckerFilledTargetErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

CryptoFunctionChecker::ResultCurveIsKnown
CryptoFunctionChecker::curveIsKnown(llvm::StringRef curve) const {
  if (!llvm::is_contained(kCurveNames, curve)) {
    auto message = ErrorTextCryptoFunction::curve(curve);

    return err(CryptoFunctionCheckerCurveIsKnownError{
        .code = CryptoFunctionCheckerCurveIsKnownErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

Expr *CryptoFunctionChecker::asyncOf(Expr *checked) {
  auto *call = dyn_cast<CallExpr>(checked);
  if (call == nullptr || call->type->isError()) {
    return checked;
  }
  if (call->builtin() == Builtin::CryptoKeyPair) {
    call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPairAsync};
    call->type = Type::promise(call->type);
    return call;
  }
  if (call->builtin() != Builtin::Crypto) {
    return checked;
  }
  auto *index = cast<IntLitExpr>(call->args[0]);
  for (const auto &[from, to] : kAsyncCryptoFns) {
    if (CryptoFn(index->value) == from) {
      index->value = uint64_t(to);
      call->type = Type::promise(call->type);
      return call;
    }
  }

  return checked;
}

Expr *CryptoFunctionChecker::checkSyncCall(CallExpr *call, llvm::StringRef functionName,
                                           llvm::StringRef shown, llvm::SMLoc loc) {
  call->type = Type::error();
  if (llvm::is_contained(kClasses, functionName)) {
    auto message = ErrorTextCryptoFunction::isClass(functionName);
    this->state.getDiagnostics().error(loc, message);
    return this->checkLoosely(call);
  }
  auto function = CryptoSyncFunctionTable::find(functionName);
  if (function == CryptoSyncFunction::CONSTANTS) {
    return this->reportConstants(call, loc);
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (!function.has_value()) {
    return this->reportUnknownFunction(call, shown, loc);
  }

  SyncCall sync{
      .call = call,
      .function = *function,
      .fn = functionName,
      .shown = shown,
      .loc = loc,
  };

  return this->checkKnownFunction(sync);
}

Expr *CryptoFunctionChecker::checkKnownFunction(SyncCall &sync) {
  switch (sync.function) {
    case CryptoSyncFunction::CONSTANTS:
      return this->reportConstants(sync.call, sync.loc);
    case CryptoSyncFunction::CREATE_HASH:
      return this->checkCreateHash(sync);
    case CryptoSyncFunction::CREATE_HMAC:
      return this->checkCreateHmac(sync);
    case CryptoSyncFunction::HASH:
      return this->checkHashOnce(sync);
    case CryptoSyncFunction::RANDOM_BYTES:
      return this->checkRandomBytes(sync);
    case CryptoSyncFunction::RANDOM_UUID:
      return this->checkRandomUuid(sync);
    case CryptoSyncFunction::RANDOM_INT:
      return this->checkRandomInt(sync);
    case CryptoSyncFunction::RANDOM_FILL_SYNC:
    case CryptoSyncFunction::GET_RANDOM_VALUES:
      return this->checkRandomFill(sync);
    case CryptoSyncFunction::TIMING_SAFE_EQUAL:
      return this->checkTimingSafeEqual(sync);
    case CryptoSyncFunction::PBKDF2_SYNC:
      return this->checkPbkdf2(sync);
    case CryptoSyncFunction::HKDF_SYNC:
      return this->checkHkdf(sync);
    case CryptoSyncFunction::SCRYPT_SYNC:
      return this->checkScrypt(sync);
    case CryptoSyncFunction::ARGON2_SYNC:
      return this->checkArgon2(sync);
    case CryptoSyncFunction::CREATE_CIPHERIV:
    case CryptoSyncFunction::CREATE_DECIPHERIV:
      return this->checkCreateCipher(sync);
    case CryptoSyncFunction::GET_HASHES:
    case CryptoSyncFunction::GET_CIPHERS:
    case CryptoSyncFunction::GET_CURVES:
      return this->checkNameList(sync);
    case CryptoSyncFunction::CREATE_SECRET_KEY:
      return this->checkCreateSecretKey(sync);
    case CryptoSyncFunction::CREATE_PUBLIC_KEY:
    case CryptoSyncFunction::CREATE_PRIVATE_KEY:
      return this->checkCreateKey(sync);
    case CryptoSyncFunction::GENERATE_KEY_PAIR_SYNC:
      return this->checkGenerateKeyPair(sync);
    case CryptoSyncFunction::GENERATE_KEY_SYNC:
      return this->checkGenerateKey(sync);
    case CryptoSyncFunction::SIGN:
    case CryptoSyncFunction::VERIFY:
      return this->checkSignOnce(sync);
    case CryptoSyncFunction::CREATE_SIGN:
    case CryptoSyncFunction::CREATE_VERIFY:
      return this->checkCreateSigner(sync);
    case CryptoSyncFunction::CREATE_ECDH:
      return this->checkCreateEcdh(sync);
    case CryptoSyncFunction::DIFFIE_HELLMAN:
      return this->checkDiffieHellman(sync);
    case CryptoSyncFunction::GENERATE_PRIME_SYNC:
      return this->checkGeneratePrime(sync);
    case CryptoSyncFunction::CHECK_PRIME_SYNC:
      return this->checkCheckPrime(sync);
  }

  return sync.call;
}

Expr *CryptoFunctionChecker::reportConstants(CallExpr *call, llvm::SMLoc loc) {
  auto message = ErrorTextCryptoValue::constants();
  this->state.getDiagnostics().error(loc, message);

  return this->checkLoosely(call);
}

Expr *CryptoFunctionChecker::reportUnknownFunction(CallExpr *call, llvm::StringRef shown,
                                                   llvm::SMLoc loc) {
  auto found = CryptoExports::cryptoExport(shown, "node:crypto");
  std::string message = found.isErr() ? found.error().message : std::string();
  this->state.getDiagnostics().error(loc, message);

  return this->checkLoosely(call);
}

Expr *CryptoFunctionChecker::checkLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

bool CryptoFunctionChecker::isPresent(const std::vector<Expr *> &args, size_t index) const {
  return index < args.size() && !isa<UndefinedExpr>(args[index]);
}

bool CryptoFunctionChecker::optionsReadAt(SyncCall &sync, size_t index,
                                          llvm::ArrayRef<llvm::StringRef> allowed) {
  std::vector<Expr *> &args = sync.call->args;
  if (!this->isPresent(args, index)) {
    return true;
  }

  return this->cryptoArgs->cryptoOptions(args[index], sync.shown, allowed, sync.options);
}

Expr *CryptoFunctionChecker::provenNumber(Expr *arg, int64_t low, std::optional<int64_t> high,
                                          llvm::StringRef what) {
  auto *checked = this->exprChecker->checkExpr(arg, Type::intTy());
  if (checked->type->isError()) {
    return nullptr;
  }
  auto between = this->indexProofs->requireBetween(checked, low, high, what);
  bool isShown =
      between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.getDiagnostics().error(checked->loc(), between.error().message);
  }
  if (between.isErr() && !this->indexProofs->isExempt(checked->loc())) {
    return nullptr;
  }
  if (checked->type->isFloat()) {
    return checked;
  }

  return this->typeRelations->convert(checked, Type::doubleTy());
}

Expr *CryptoFunctionChecker::checkCreateHash(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *md = this->cryptoArgs->cryptoMd(args[0], false);
  if (!this->optionsReadAt(sync, 1, {"outputLength"}) || md == nullptr) {
    return call;
  }
  Expr *length = options.lookup("outputLength");
  std::vector<Expr *> hashArgs = {md, length != nullptr ? this->cryptoArgs->cryptoNumber(length)
                                                        : this->cryptoArgs->cryptoDouble(loc, -1)};
  return this->cryptoArgs->cryptoResult(call, CryptoFn::HashNew, std::move(hashArgs),
                                        Type::cryptoType(Crypto::Hash));
}

Expr *CryptoFunctionChecker::checkCreateHmac(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *md = this->cryptoArgs->cryptoMd(args[0], true);
  auto *key = this->cryptoArgs->cryptoData(args[1], "key", true, true, true);
  if (md == nullptr || key->type->isError()) {
    return call;
  }
  std::vector<Expr *> hmacArgs = {md, key, this->cryptoArgs->cryptoInt(loc, 0)};
  return this->cryptoArgs->cryptoResult(call, CryptoFn::HmacNew, std::move(hmacArgs),
                                        Type::cryptoType(Crypto::Hmac));
}

Expr *CryptoFunctionChecker::checkHashOnce(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *md = this->cryptoArgs->cryptoMd(args[0], false);
  auto *data = this->cryptoArgs->cryptoData(args[1], "data", true, false, false);
  auto *encoding = this->cryptoArgs->cryptoEncoding(call, 2, functionName, 6);
  if (md == nullptr || data->type->isError() || encoding == nullptr) {
    return call;
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    return this->cryptoArgs->cryptoResult(call, CryptoFn::HashOnce, {md, data}, buffer);
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::HashOnceText, {md, data, encoding},
                                        Type::string());
}

Expr *CryptoFunctionChecker::checkRandomBytes(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || this->cryptoArgs->rejectCryptoCallback(call, 1, functionName,
                                                              "'const bytes = randomBytes(n)'")) {
    return call;
  }
  auto *size = this->provenNumber(args[0], 0, 2147483647, "the size");
  if (size == nullptr) {
    return call;
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomBytes, {size}, buffer);
}

Expr *CryptoFunctionChecker::checkRandomUuid(SyncCall &sync) {
  CallExpr *call = sync.call;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || !this->optionsReadAt(sync, 0, {"disableEntropyCache"})) {
    return call;
  }
  Expr *cache = options.lookup("disableEntropyCache");
  if (cache != nullptr) {
    this->argumentChecker->checkArg(cache, Type::boolTy());
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
}

Expr *CryptoFunctionChecker::checkRandomInt(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  size_t last = args.size() - 1;
  if (args.size() > 1 && this->cryptoArgs->rejectCryptoCallback(call, last, functionName,
                                                                "'const n = randomInt(max)'")) {
    return call;
  }
  if (args.size() == 3) {
    auto message = ErrorTextCryptoFunction::randomArgs();
    this->state.getDiagnostics().error(args[2]->loc(), message);
    return this->checkLoosely(call);
  }
  Expr *min = args.size() == 2 ? this->exprChecker->checkExpr(args[0], Type::intTy())
                               : this->cryptoArgs->cryptoInt(loc, 0);
  auto *max = this->exprChecker->checkExpr(args.back(), Type::intTy());
  if (min->type->isError() || max->type->isError()) {
    return call;
  }
  for (Expr *bound : {min, max}) {
    if (!bound->type->isInt()) {
      auto message = ErrorTextCryptoFunction::randomInt(bound->type->name());
      this->state.getDiagnostics().error(bound->loc(), message);
      return call;
    }
  }
  auto greater = ErrorTextCryptoFunction::greater(args.size() == 2);
  auto less = this->indexProofs->requireLess(min, max, greater);
  if (less.isErr()) {
    this->state.getDiagnostics().error(max->loc(), less.error().message);
    return call;
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
}

Expr *CryptoFunctionChecker::checkRandomFill(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  unsigned maxArgs = sync.function == CryptoSyncFunction::RANDOM_FILL_SYNC ? 3 : 1;
  auto arity = this->argumentChecker->checkArity(call, 1, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *target = this->exprChecker->checkExpr(args[0]);
  const Type *type = target->type;
  if (type->isError()) {
    return call;
  }
  auto filled = this->filledTarget(type, sync.fn, sync.shown);
  if (filled.isErr()) {
    this->state.getDiagnostics().error(target->loc(), filled.error().message);
    return call;
  }
  if (sync.function == CryptoSyncFunction::GET_RANDOM_VALUES) {
    return this->cryptoArgs->cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
  }

  bool isOffsetGiven = this->isPresent(args, 1);
  bool isSizeGiven = this->isPresent(args, 2);
  int has = (isOffsetGiven ? 1 : 0) | (isSizeGiven ? 2 : 0);
  auto *offset = this->cryptoArgs->cryptoDouble(loc, 0);
  auto *size = this->cryptoArgs->cryptoDouble(loc, 0);
  if (isOffsetGiven) {
    auto *at = this->provenFillOffset(args[1], target);
    if (at == nullptr) {
      return call;
    }
    if (isSizeGiven) {
      auto *count = this->provenFillSize(args[2], at, target);
      if (count == nullptr) {
        return call;
      }
      size = this->toDouble(count);
    }
    offset = this->toDouble(at);
  }
  std::vector<Expr *> fillArgs{target, offset, size, this->cryptoArgs->cryptoInt(loc, has)};

  return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomFill, std::move(fillArgs), type);
}

Expr *CryptoFunctionChecker::provenFillOffset(Expr *arg, const Expr *target) {
  auto *at = this->exprChecker->checkExpr(arg, Type::intTy());
  bool isLibrary = this->indexProofs->isExempt(at->loc());
  if (at->type->isError()) {
    return nullptr;
  }
  if (isLibrary) {
    return at;
  }
  auto offsetWhat = ErrorTextCryptoFnLabel::offset();
  auto between = this->indexProofs->requireBetween(at, 0, {}, offsetWhat);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(at->loc(), between.error().message);
  }
  if (between.isErr()) {
    return nullptr;
  }
  auto upTo = this->indexProofs->requireUpTo(at, target, 0, offsetWhat);
  if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(at->loc(), upTo.error().message);
  }
  if (upTo.isErr()) {
    return nullptr;
  }

  return at;
}

Expr *CryptoFunctionChecker::provenFillSize(Expr *arg, Expr *at, const Expr *target) {
  auto *count = this->exprChecker->checkExpr(arg, Type::intTy());
  if (count->type->isError()) {
    return nullptr;
  }
  bool isLibrary = this->indexProofs->isExempt(at->loc());
  if (!isLibrary) {
    auto sizeWhat = ErrorTextCryptoFnLabel::size();
    auto between = this->indexProofs->requireBetween(count, 0, {}, sizeWhat);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(count->loc(), between.error().message);
    }
    if (between.isErr()) {
      return nullptr;
    }
  }
  auto start = this->indexProofs->linear(at);
  auto length = this->indexProofs->linear(count);
  bool isFitting = this->indexProofs->canProveSumUpTo(start, length, target) ||
                   this->indexProofs->isExempt(count->loc());
  if (!isFitting) {
    auto countText = BytesCheckHelpers::valueText(count);
    auto atText = BytesCheckHelpers::valueText(at);
    auto message = ErrorTextCryptoFunction::fillFits(countText, atText);
    this->state.getDiagnostics().error(count->loc(), message);
    return nullptr;
  }

  return count;
}

Expr *CryptoFunctionChecker::toDouble(Expr *number) {
  if (number->type->isFloat()) {
    return number;
  }

  return this->typeRelations->convert(number, Type::doubleTy());
}

Expr *CryptoFunctionChecker::checkTimingSafeEqual(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto firstWhat = ErrorTextCryptoFnLabel::first();
  auto secondWhat = ErrorTextCryptoFnLabel::second();
  auto *a = this->cryptoArgs->cryptoData(args[0], firstWhat, false, false, true);
  auto *b = this->cryptoArgs->cryptoData(args[1], secondWhat, false, false, true);
  if (a->type->isError() || b->type->isError()) {
    return call;
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b}, Type::boolTy());
}

Expr *CryptoFunctionChecker::checkPbkdf2(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 5, 5);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *password = this->cryptoArgs->cryptoData(args[0], "password", true, true, true);
  auto *salt = this->cryptoArgs->cryptoData(args[1], "salt", true, false, true);
  Expr *iterations = this->cryptoArgs->cryptoNumber(args[2]),
       *keylen = this->cryptoArgs->cryptoNumber(args[3]);
  auto *md = this->cryptoArgs->cryptoMd(args[4], true);
  if (md == nullptr || password->type->isError() || salt->type->isError() ||
      !this->cryptoArgs->checkWhole(iterations, "iterations", 1) ||
      !this->cryptoArgs->checkWhole(keylen, "keylen", 1)) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::Pbkdf2,
                                        {password, salt, iterations, keylen, md}, buffer);
}

Expr *CryptoFunctionChecker::checkHkdf(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 5, 5);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *md = this->cryptoArgs->cryptoMd(args[0], true);
  auto *key = this->cryptoArgs->cryptoData(args[1], "ikm", true, true, true);
  auto *salt = this->cryptoArgs->cryptoData(args[2], "salt", true, false, true);
  auto *info = this->cryptoArgs->cryptoData(args[3], "info", true, false, true);
  auto *keylen = this->cryptoArgs->cryptoNumber(args[4]);
  if (md == nullptr || key->type->isError() || salt->type->isError() || info->type->isError() ||
      !this->cryptoArgs->checkWhole(keylen, "keylen", 1)) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                                        Type::bytesType(Type::Bytes::ArrayBuffer));
}

Expr *CryptoFunctionChecker::checkScrypt(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 3, 4);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *password = this->cryptoArgs->cryptoData(args[0], "password", true, true, true);
  auto *salt = this->cryptoArgs->cryptoData(args[1], "salt", true, false, true);
  auto *keylen = this->cryptoArgs->cryptoNumber(args[2]);
  if (!this->optionsReadAt(sync, 3,
                           {"N", "r", "p", "maxmem", "cost", "blockSize", "parallelization"})) {
    return call;
  }
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> pairs[] = {
      {"N", "cost"}, {"r", "blockSize"}, {"p", "parallelization"}};
  Expr *values[3];
  for (int index = 0; index < 3; ++index) {
    Expr *a = options.lookup(pairs[index].first);
    Expr *b = options.lookup(pairs[index].second);
    if (a != nullptr && b) {
      auto message = ErrorTextCryptoFunction::together(pairs[index].first, pairs[index].second);
      this->state.getDiagnostics().error(b->loc(), message);
      return call;
    }
    values[index] = a != nullptr || b ? this->cryptoArgs->cryptoNumber(a != nullptr ? a : b)
                                      : this->cryptoArgs->cryptoDouble(loc, 0);
  }
  if (std::optional<double> cost = this->cryptoArgs->knownNumber(values[0])) {
    int64_t n = int64_t(*cost);
    if (*cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0)) {
      auto costShown = this->cryptoArgs->numberText(*cost);
      auto message = ErrorTextCryptoFunction::costN(costShown);
      this->state.getDiagnostics().error(values[0]->loc(), message);
      return call;
    }
  }
  Expr *maxmem = options.lookup("maxmem");
  if (password->type->isError() || salt->type->isError()) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::Scrypt,
                                        {password, salt, keylen, values[0], values[1], values[2],
                                         maxmem != nullptr
                                             ? this->cryptoArgs->cryptoNumber(maxmem)
                                             : this->cryptoArgs->cryptoDouble(loc, 0)},
                                        buffer);
}

bool CryptoFunctionChecker::argonOptionsGiven(SyncCall &sync) {
  std::vector<Expr *> &args = sync.call->args;
  bool isRead = this->cryptoArgs->cryptoOptions(args[1], sync.shown,
                                                {"message", "nonce", "parallelism", "tagLength",
                                                 "memory", "passes", "secret", "associatedData"},
                                                sync.options);
  if (!isRead) {
    return false;
  }
  for (llvm::StringRef required :
       {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
    if (sync.options.lookup(required) == nullptr) {
      auto message = ErrorTextCryptoFunction::needs(sync.shown, required);
      this->state.getDiagnostics().error(args[1]->loc(), message);
      return false;
    }
  }

  return true;
}

Expr *CryptoFunctionChecker::checkArgon2(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *type, *name;
  if (this->literalOf(args[0])) {
    auto argon = ErrorTextCryptoFnLabel::argon();
    int which = this->cryptoArgs->cryptoChoice(args[0], argon, {"argon2d", "argon2i", "argon2id"});
    if (which < 0) {
      return call;
    }
    type = this->cryptoArgs->cryptoInt(loc, which);
    name = this->cryptoArgs->cryptoString(loc, "");
  } else {
    type = this->cryptoArgs->cryptoInt(loc, 0xffffffff);
    name = this->argumentChecker->checkArg(args[0], Type::string());
  }
  if (!this->argonOptionsGiven(sync)) {
    return call;
  }
  auto *message = this->cryptoArgs->cryptoData(options["message"], "message", true, false, true);
  auto *nonce = this->cryptoArgs->cryptoData(options["nonce"], "nonce", true, false, true);
  auto *parallelism = this->cryptoArgs->cryptoNumber(options["parallelism"]);
  auto *tagLength = this->cryptoArgs->cryptoNumber(options["tagLength"]);
  auto *memory = this->cryptoArgs->cryptoNumber(options["memory"]);
  auto *passes = this->cryptoArgs->cryptoNumber(options["passes"]);
  Expr *secret = options.lookup("secret")
                     ? this->cryptoArgs->cryptoData(options["secret"], "secret", true, false, true)
                     : this->cryptoArgs->cryptoUndefined(loc);
  Expr *data = options.lookup("associatedData")
                   ? this->cryptoArgs->cryptoData(options["associatedData"], "associatedData", true,
                                                  false, true)
                   : this->cryptoArgs->cryptoUndefined(loc);
  for (Expr *expr : {message, nonce, secret, data}) {
    if (expr->type->isError()) {
      return call;
    }
  }
  return this->cryptoArgs->cryptoResult(
      call, CryptoFn::Argon2,
      {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
}

Expr *CryptoFunctionChecker::checkCreateCipher(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  auto arity = this->argumentChecker->checkArity(call, 3, 4);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto name = this->literalOf(args[0]);
  auto *cipher = this->cryptoArgs->cryptoCipher(args[0]);
  auto *key = this->cryptoArgs->cryptoData(args[1], "key", true, true, true);
  auto *iv = this->cryptoArgs->cryptoData(args[2], "iv", true, false, true);
  if (!this->optionsReadAt(sync, 3, {"authTagLength"}) || cipher == nullptr ||
      key->type->isError() || iv->type->isError() ||
      !this->cryptoArgs->checkCipherLengths(cipher, key, iv)) {
    return call;
  }
  Expr *tag = options.lookup("authTagLength");
  if (tag == nullptr && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
    auto message = ErrorTextCryptoFunction::tagLength(*name);
    this->state.getDiagnostics().error(args[0]->loc(), message);
    return call;
  }
  bool isDecrypt = sync.function == CryptoSyncFunction::CREATE_DECIPHERIV;
  return this->cryptoArgs->cryptoResult(
      call, CryptoFn::CipherNew,
      {cipher, key, iv,
       tag != nullptr ? this->cryptoArgs->cryptoNumber(tag)
                      : this->cryptoArgs->cryptoDouble(loc, -1),
       this->cryptoArgs->cryptoBool(loc, isDecrypt)},
      Type::cryptoType(isDecrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
}

Expr *CryptoFunctionChecker::checkNameList(SyncCall &sync) {
  CallExpr *call = sync.call;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  return this->cryptoArgs->cryptoCall(
      call,
      sync.function == CryptoSyncFunction::GET_HASHES    ? CryptoFn::GetHashes
      : sync.function == CryptoSyncFunction::GET_CIPHERS ? CryptoFn::GetCiphers
                                                         : CryptoFn::GetCurves,
      {}, Type::array(Type::string()));
}

Expr *CryptoFunctionChecker::checkCreateSecretKey(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  const auto *keyObject = Type::cryptoType(Crypto::KeyObject);
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *key = this->cryptoArgs->cryptoData(args[0], "key", true, false, true);
  auto *encoding = this->cryptoArgs->cryptoEncoding(call, 1, functionName, 0);
  if (key->type->isError() || encoding == nullptr) {
    return call;
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    encoding = this->cryptoArgs->cryptoInt(loc, 0);
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
}

Expr *CryptoFunctionChecker::checkCreateKey(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  bool isPriv = sync.function == CryptoSyncFunction::CREATE_PRIVATE_KEY;
  KeyInput input{
      .key = nullptr,
      .encoding = this->cryptoArgs->cryptoInt(loc, 0),
      .type = this->cryptoArgs->cryptoString(loc, ""),
      .passphrase = this->cryptoArgs->cryptoUndefined(loc),
      .format = 0,
      .has = 0,
  };
  if (isa<ObjectLitExpr>(args[0])) {
    if (!this->keyOptionsRead(sync, isPriv, input)) {
      return call;
    }
  } else {
    input.key = this->cryptoArgs->cryptoData(args[0], "key", true, !isPriv, true);
  }
  if (input.key->type->isError() || input.passphrase->type->isError()) {
    return call;
  }
  const auto *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> keyArgs{this->cryptoArgs->cryptoBool(loc, isPriv),
                              input.key,
                              input.encoding,
                              this->cryptoArgs->cryptoInt(loc, input.format),
                              this->cryptoArgs->cryptoInt(loc, input.has),
                              input.type,
                              input.passphrase};

  return this->cryptoArgs->cryptoResult(call, CryptoFn::CreateKey, std::move(keyArgs), keyObject);
}

bool CryptoFunctionChecker::keyOptionsRead(SyncCall &sync, bool priv, KeyInput &input) {
  Expr *given = sync.call->args[0];
  llvm::StringMap<Expr *> &options = sync.options;
  bool isRead = this->cryptoArgs->cryptoOptions(
      given, sync.fn, {"key", "format", "type", "passphrase", "encoding"}, options);
  if (!isRead) {
    return false;
  }
  Expr *format = options.lookup("format");
  if (format != nullptr) {
    auto keyFormat = ErrorTextCryptoFnLabel::keyFormat();
    input.format = this->cryptoArgs->cryptoChoice(format, keyFormat, {"pem", "der", "jwk"});
    if (input.format < 0) {
      return false;
    }
  }
  Expr *key = options.lookup("key");
  if (key == nullptr) {
    auto message = ErrorTextCryptoOptions::needsKey(sync.fn);
    this->state.getDiagnostics().error(given->loc(), message);
    return false;
  }
  input.key = input.format == 2 ? this->argumentChecker->checkArg(key, Type::json())
                                : this->cryptoArgs->cryptoData(key, "key", true, !priv, true);

  return this->keyDetailsRead(options, input);
}

bool CryptoFunctionChecker::keyDetailsRead(llvm::StringMap<Expr *> &options, KeyInput &input) {
  Expr *type = options.lookup("type");
  if (type != nullptr) {
    auto keyType = ErrorTextCryptoFnLabel::keyType();
    int which = this->cryptoArgs->cryptoChoice(type, keyType, {"pkcs1", "spki", "pkcs8", "sec1"});
    if (which < 0) {
      return false;
    }
    static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
    input.type = this->cryptoArgs->cryptoString(type->loc(), types[which]);
    input.has |= 1;
  }
  Expr *passphrase = options.lookup("passphrase");
  if (passphrase != nullptr) {
    input.passphrase = this->cryptoArgs->cryptoData(passphrase, "passphrase", true, false, true);
    input.has |= 2;
  }
  Expr *encoding = options.lookup("encoding");
  if (encoding == nullptr) {
    return true;
  }
  bool isLiteral = this->literalOf(encoding).has_value();
  Expr *number = isLiteral ? this->bytesChecker->checkBufferEncoding(encoding) : nullptr;
  if (number == nullptr || !isa<IntLitExpr>(number)) {
    auto message = ErrorTextCryptoFunction::keyCoding();
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  input.encoding = number;

  return true;
}

Expr *CryptoFunctionChecker::checkGenerateKeyPair(SyncCall &sync) {
  CallExpr *call = sync.call;
  llvm::StringRef shown = sync.shown;
  llvm::SMLoc loc = sync.loc;
  return this->keyPairChecker->checkGenerateKeyPair(call, loc, shown);
}

Expr *CryptoFunctionChecker::checkGenerateKey(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef shown = sync.shown;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  const auto *keyObject = Type::cryptoType(Crypto::KeyObject);
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto keyType = ErrorTextCryptoFnLabel::keyType();
  int which = this->cryptoArgs->cryptoChoice(args[0], keyType, {"hmac", "aes"});
  if (which < 0 || !this->cryptoArgs->cryptoOptions(args[1], shown, {"length"}, options)) {
    return call;
  }
  if (!options.lookup("length")) {
    auto message = ErrorTextCryptoFunction::keyLength(shown);
    this->state.getDiagnostics().error(args[1]->loc(), message);
    return call;
  }
  auto *bits = this->exprChecker->checkExpr(options["length"], Type::intTy());
  if (bits->type->isError()) {
    return call;
  }
  if (which == 1) {
    auto known = this->constantFolder->intConstant(bits);
    if (!known || (*known != 128 && *known != 192 && *known != 256)) {
      auto message = ErrorTextCryptoFunction::aesLength();
      this->state.getDiagnostics().error(bits->loc(), message);
      return call;
    }
  } else {
    auto keyBits = ErrorTextCryptoFnLabel::keyBits();
    auto between = this->indexProofs->requireBetween(bits, 8, {}, keyBits);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(bits->loc(), between.error().message);
    }
    if (between.isErr() && !this->indexProofs->isExempt(bits->loc())) {
      return call;
    }
  }
  return this->cryptoArgs->cryptoCall(
      call, CryptoFn::GenerateSecretKey,
      {this->cryptoArgs->cryptoBool(loc, which == 1),
       bits->type->isFloat() ? bits : this->typeRelations->convert(bits, Type::doubleTy())},
      keyObject);
}

Expr *CryptoFunctionChecker::checkSignOnce(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  bool isVerify = sync.function == CryptoSyncFunction::VERIFY;
  size_t count = isVerify ? 4 : 3;
  auto arity = this->argumentChecker->checkArity(call, unsigned(count), unsigned(count + 1));
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || this->cryptoArgs->rejectCryptoCallback(
                           call, count, functionName,
                           isVerify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'")) {
    return call;
  }
  Expr *algorithm = isa<UndefinedExpr>(args[0])
                        ? this->cryptoArgs->cryptoUndefined(loc)
                        : this->argumentChecker->checkArg(args[0], Type::string());
  auto *data = this->cryptoArgs->cryptoData(args[1], "data", true, false, true);
  std::vector<Expr *> signArgs = {algorithm, data};
  if (!this->cryptoArgs->cryptoSignKey(args[2], functionName, !isVerify, signArgs)) {
    return call;
  }
  if (isVerify) {
    auto *signature = this->cryptoArgs->cryptoData(args[3], "signature", true, false, true);
    if (signature->type->isError()) {
      return call;
    }
    signArgs.insert(signArgs.begin() + 3, signature);
  }
  if (data->type->isError()) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, isVerify ? CryptoFn::Verify : CryptoFn::Sign,
                                        std::move(signArgs), isVerify ? Type::boolTy() : buffer);
}

Expr *CryptoFunctionChecker::checkCreateSigner(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *name = args[0];
  bool isLiteral = this->literalOf(name).has_value();
  if (isLiteral && this->cryptoArgs->cryptoMd(name, true) == nullptr) {
    return call;
  }
  bool isVerify = sync.function == CryptoSyncFunction::CREATE_VERIFY;
  std::vector<Expr *> signArgs = {this->argumentChecker->checkArg(name, Type::string()),
                                  this->cryptoArgs->cryptoBool(loc, isVerify)};
  const auto *made = Type::cryptoType(isVerify ? Crypto::Verify : Crypto::Sign);
  return this->cryptoArgs->cryptoResult(call, CryptoFn::SignNew, std::move(signArgs), made);
}

Expr *CryptoFunctionChecker::checkCreateEcdh(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto curve = this->literalOf(args[0]);
  auto known = curve ? this->curveIsKnown(*curve) : ok(llvm::StringRef());
  if (known.isErr()) {
    this->state.getDiagnostics().error(args[0]->loc(), known.error().message);
    return this->checkLoosely(call);
  }
  auto *name = this->argumentChecker->checkArg(args[0], Type::string());
  return this->cryptoArgs->cryptoResult(call, CryptoFn::EcdhNew, {name},
                                        Type::cryptoType(Crypto::Ecdh));
}

Expr *CryptoFunctionChecker::checkDiffieHellman(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef functionName = sync.fn;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || !this->cryptoArgs->cryptoOptions(args[0], functionName,
                                                        {"privateKey", "publicKey"}, options)) {
    return call;
  }
  if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
    auto message = ErrorTextCryptoFunction::dhKeys();
    this->state.getDiagnostics().error(args[0]->loc(), message);
    return call;
  }
  auto privateWhat = ErrorTextCryptoFnLabel::privateKey();
  auto publicWhat = ErrorTextCryptoFnLabel::publicKey();
  auto *priv = this->cryptoArgs->cryptoKeyObject(options["privateKey"], privateWhat);
  auto *pub = this->cryptoArgs->cryptoKeyObject(options["publicKey"], publicWhat);
  if (priv->type->isError() || pub->type->isError()) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
}

Expr *CryptoFunctionChecker::checkGeneratePrime(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || !this->optionsReadAt(sync, 1, {"add", "rem", "safe", "bigint"})) {
    return call;
  }
  Expr *big = options.lookup("bigint");
  if (big != nullptr) {
    auto message = ErrorTextCryptoFunction::bigint();
    this->state.getDiagnostics().error(big->loc(), message);
    return call;
  }
  auto *size = this->cryptoArgs->cryptoNumber(args[0]);
  Expr *safe = options.lookup("safe")
                   ? this->argumentChecker->checkArg(options["safe"], Type::boolTy())
                   : this->cryptoArgs->cryptoBool(loc, false);
  Expr *add = options.lookup("add")
                  ? this->cryptoArgs->cryptoData(options["add"], "add", false, false, true)
                  : this->cryptoArgs->cryptoUndefined(loc);
  Expr *rem = options.lookup("rem")
                  ? this->cryptoArgs->cryptoData(options["rem"], "rem", false, false, true)
                  : this->cryptoArgs->cryptoUndefined(loc);
  if (size->type->isError() || safe->type->isError() || add->type->isError() ||
      rem->type->isError() || !this->cryptoArgs->checkWhole(size, "size", 1)) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::GeneratePrime, {size, safe, add, rem},
                                        Type::bytesType(Type::Bytes::ArrayBuffer));
}

Expr *CryptoFunctionChecker::checkCheckPrime(SyncCall &sync) {
  CallExpr *call = sync.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = sync.loc;
  llvm::StringMap<Expr *> &options = sync.options;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (arity.isErr() || !this->optionsReadAt(sync, 1, {"checks"})) {
    return call;
  }
  auto *candidate = this->cryptoArgs->cryptoData(args[0], "candidate", false, false, true);
  Expr *checks = options.lookup("checks") ? this->cryptoArgs->cryptoNumber(options["checks"])
                                          : this->cryptoArgs->cryptoDouble(loc, 0);
  if (candidate->type->isError() || checks->type->isError() ||
      !this->cryptoArgs->checkWhole(checks, "checks", 0)) {
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::CheckPrime, {candidate, checks},
                                        Type::boolTy());
}

}
