#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/library/crypto/key_pair_checker/key_pair_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::string> literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return utf8Of(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return utf8Of(tmpl->quasis[0]);
  }

  return std::nullopt;
}

}

CryptoFunctionChecker::CryptoFunctionChecker(Checker &checker) : checker_(checker) {}

Expr *CryptoFunctionChecker::checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  llvm::StringRef sync = fn;
  bool async = false;
  for (const auto &[form, syncName] : kAsyncForms) {
    if (fn == form) {
      async = true;
      sync = syncName;
    }
  }
  for (const auto &[form, syncName] : kTzCryptoAsyncForms) {
    if (fn == form) {
      async = true;
      sync = syncName;
    }
  }
  if (async && !call->args.empty() && isa<FunctionExpr>(call->args.back())) {
    checker_.diags_.error(call->args.back()->loc(),
                          "'" + fn +
                              "' with a callback is not supported: tz has no callbacks "
                              "(docs/crypto.md, section 5); without one it returns a Promise: "
                              "'const result = await " +
                              fn + "(...)'");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  Expr *checked = checkSyncCall(call, sync, fn, loc);

  return async ? asyncOf(checked) : checked;
}

Expr *CryptoFunctionChecker::asyncOf(Expr *checked) {
  auto *call = dyn_cast<CallExpr>(checked);
  if (!call || call->type->isError()) {
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

Expr *CryptoFunctionChecker::checkSyncCall(CallExpr *call, llvm::StringRef fn,
                                           llvm::StringRef shown, llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    checker_.diags_.error(
        loc, "'" + fn + "' of 'node:crypto' is a class, not a function" +
                 (fn == "X509Certificate" ? "; make one with 'X509Certificate.parse(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    checker_.diags_.error(
        loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
             "and the like");
    return loosely();
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || checker_.cryptoArgs().cryptoOptions(args[i], shown, allowed, options);
  };

  if (fn == "createHash") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    std::vector<Expr *> hashArgs = {md, length ? checker_.cryptoArgs().cryptoNumber(length)
                                               : checker_.cryptoArgs().cryptoDouble(loc, -1)};
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::HashNew, std::move(hashArgs),
                                              Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    std::vector<Expr *> hmacArgs = {md, key, checker_.cryptoArgs().cryptoInt(loc, 0)};
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::HmacNew, std::move(hmacArgs),
                                              Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 2, 3);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], false);
    Expr *data = checker_.cryptoArgs().cryptoData(args[1], "data", true, false, false);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 2, fn, 6);
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return checker_.cryptoArgs().cryptoResult(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::HashOnceText, {md, data, encoding},
                                              Type::string());
  }
  auto proven = [&](Expr *arg, int64_t lo, std::optional<int64_t> hi,
                    llvm::StringRef what) -> Expr * {
    Expr *e = checker_.exprChecker().checkExpr(arg, Type::intTy());
    if (e->type->isError()) {
      return nullptr;
    }
    IndexProofs::ResultBetweenProven between =
        checker_.indexProofs().requireBetween(e, lo, hi, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      checker_.diags_.error(e->loc(), between.error().message);
    }
    if (between.isErr() && !checker_.indexProofs().exempt(e->loc())) {
      return nullptr;
    }
    return e->type->isFloat() ? e : checker_.typeRelations().convert(e, Type::doubleTy());
  };
  if (fn == "randomBytes") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        checker_.cryptoArgs().rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    Expr *size = proven(args[0], 0, 2147483647, "the size");
    if (!size) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomBytes, {size}, buffer);
  }
  if (fn == "randomUUID") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      checker_.argumentChecker().checkArg(cache, Type::boolTy());
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 3);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 &&
        checker_.cryptoArgs().rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      checker_.diags_.error(args[2]->loc(), "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? checker_.exprChecker().checkExpr(args[0], Type::intTy())
                                 : checker_.cryptoArgs().cryptoInt(loc, 0);
    Expr *max = checker_.exprChecker().checkExpr(args.back(), Type::intTy());
    if (min->type->isError() || max->type->isError()) {
      return call;
    }
    for (Expr *bound : {min, max}) {
      if (!bound->type->isInt()) {
        checker_.diags_.error(bound->loc(), "'randomInt' takes whole numbers ('int'), found '" +
                                                bound->type->name() + "'");
        return call;
      }
    }
    IndexProofs::ResultLessWasProven less = checker_.indexProofs().requireLess(
        min, max,
        args.size() == 2 ? "the maximum is greater than the minimum"
                         : "the maximum is greater than 0");
    if (less.isErr()) {
      checker_.diags_.error(max->loc(), less.error().message);
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    ArgumentChecker::ResultArityMatches arity =
        checker_.argumentChecker().checkArity(call, 1, fn == "randomFillSync" ? 3 : 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *target = checker_.exprChecker().checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        checker_.diags_.error(target->loc(),
                              "'getRandomValues' fills an array of integers (a Uint8Array, "
                              "an Int32Array, a Buffer...); found '" +
                                  type->name() + "'");
        return call;
      }
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      checker_.diags_.error(target->loc(),
                            "'" + shown +
                                "' fills bytes (a Buffer, a typed array, a "
                                "DataView or an ArrayBuffer); found '" +
                                type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = checker_.cryptoArgs().cryptoDouble(loc, 0);
    Expr *size = checker_.cryptoArgs().cryptoDouble(loc, 0);
    if (present(1)) {
      Expr *at = checker_.exprChecker().checkExpr(args[1], Type::intTy());
      bool library = checker_.indexProofs().exempt(at->loc());
      if (at->type->isError()) {
        return call;
      }
      if (!library) {
        IndexProofs::ResultBetweenProven between =
            checker_.indexProofs().requireBetween(at, 0, {}, "the offset");
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          checker_.diags_.error(at->loc(), between.error().message);
        }
        if (between.isErr()) {
          return call;
        }
        IndexProofs::ResultUpToWasProven upTo =
            checker_.indexProofs().requireUpTo(at, target, 0, "the offset");
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          checker_.diags_.error(at->loc(), upTo.error().message);
        }
        if (upTo.isErr()) {
          return call;
        }
      }
      Expr *count = nullptr;
      if (present(2)) {
        count = checker_.exprChecker().checkExpr(args[2], Type::intTy());
        if (count->type->isError()) {
          return call;
        }
        if (!library) {
          IndexProofs::ResultBetweenProven sizeBetween =
              checker_.indexProofs().requireBetween(count, 0, {}, "the size");
          if (sizeBetween.isErr() &&
              sizeBetween.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
            checker_.diags_.error(count->loc(), sizeBetween.error().message);
          }
          if (sizeBetween.isErr()) {
            return call;
          }
        }
        if (!checker_.indexProofs().provesSumUpTo(checker_.indexProofs().linear(at),
                                                  checker_.indexProofs().linear(count), target) &&
            !checker_.indexProofs().exempt(count->loc())) {
          checker_.diags_.error(count->loc(),
                                "cannot prove that the offset and the size fit in the bytes, "
                                "and JavaScript would throw a RangeError; check it first, e.g. "
                                "'if (" +
                                    valueText(count) + " <= buf.length - " + valueText(at) + ")'");
          return call;
        }
        size = count->type->isFloat() ? count
                                      : checker_.typeRelations().convert(count, Type::doubleTy());
      }
      offset = at->type->isFloat() ? at : checker_.typeRelations().convert(at, Type::doubleTy());
    }
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::RandomFill,
        {target, offset, size, checker_.cryptoArgs().cryptoInt(loc, has)}, type);
  }
  if (fn == "timingSafeEqual") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *a = checker_.cryptoArgs().cryptoData(args[0], "first argument", false, false, true);
    Expr *b = checker_.cryptoArgs().cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b},
                                            Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 5, 5);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *password = checker_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = checker_.cryptoArgs().cryptoNumber(args[2]),
         *keylen = checker_.cryptoArgs().cryptoNumber(args[3]);
    Expr *md = checker_.cryptoArgs().cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError() ||
        !checker_.cryptoArgs().checkWhole(iterations, "iterations", 1) ||
        !checker_.cryptoArgs().checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::Pbkdf2,
                                              {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 5, 5);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[2], "salt", true, false, true);
    Expr *info = checker_.cryptoArgs().cryptoData(args[3], "info", true, false, true);
    Expr *keylen = checker_.cryptoArgs().cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError() ||
        !checker_.cryptoArgs().checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                                              Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 3, 4);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *password = checker_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = checker_.cryptoArgs().cryptoNumber(args[2]);
    if (!optionsAt(3, {"N", "r", "p", "maxmem", "cost", "blockSize", "parallelization"})) {
      return call;
    }
    static const std::pair<llvm::StringLiteral, llvm::StringLiteral> pairs[] = {
        {"N", "cost"}, {"r", "blockSize"}, {"p", "parallelization"}};
    Expr *values[3];
    for (int i = 0; i < 3; ++i) {
      Expr *a = options.lookup(pairs[i].first), *b = options.lookup(pairs[i].second);
      if (a && b) {
        checker_.diags_.error(b->loc(), "option '" + pairs[i].first +
                                            "' cannot be used together with '" + pairs[i].second +
                                            "': they are one option");
        return call;
      }
      values[i] = a || b ? checker_.cryptoArgs().cryptoNumber(a ? a : b)
                         : checker_.cryptoArgs().cryptoDouble(loc, 0);
    }
    if (std::optional<double> cost = checker_.cryptoArgs().knownNumber(values[0])) {
      int64_t n = int64_t(*cost);
      if (*cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0)) {
        checker_.diags_.error(values[0]->loc(), "N must be a power of two greater than 1, not " +
                                                    CryptoArgs::numberText(*cost));
        return call;
      }
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::Scrypt,
                                              {password, salt, keylen, values[0], values[1],
                                               values[2],
                                               maxmem ? checker_.cryptoArgs().cryptoNumber(maxmem)
                                                      : checker_.cryptoArgs().cryptoDouble(loc, 0)},
                                              buffer);
  }
  if (fn == "argon2Sync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *type, *name;
    if (literalOf(args[0])) {
      int which = checker_.cryptoArgs().cryptoChoice(args[0], "algorithm of Argon2",
                                                     {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = checker_.cryptoArgs().cryptoInt(loc, which);
      name = checker_.cryptoArgs().cryptoString(loc, "");
    } else {
      type = checker_.cryptoArgs().cryptoInt(loc, 0xffffffff);
      name = checker_.argumentChecker().checkArg(args[0], Type::string());
    }
    if (!checker_.cryptoArgs().cryptoOptions(args[1], shown,
                                             {"message", "nonce", "parallelism", "tagLength",
                                              "memory", "passes", "secret", "associatedData"},
                                             options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        checker_.diags_.error(args[1]->loc(),
                              "the parameters of '" + shown + "' need '" + required + "'");
        return call;
      }
    }
    Expr *message =
        checker_.cryptoArgs().cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = checker_.cryptoArgs().cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = checker_.cryptoArgs().cryptoNumber(options["parallelism"]);
    Expr *tagLength = checker_.cryptoArgs().cryptoNumber(options["tagLength"]);
    Expr *memory = checker_.cryptoArgs().cryptoNumber(options["memory"]);
    Expr *passes = checker_.cryptoArgs().cryptoNumber(options["passes"]);
    Expr *secret =
        options.lookup("secret")
            ? checker_.cryptoArgs().cryptoData(options["secret"], "secret", true, false, true)
            : checker_.cryptoArgs().cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? checker_.cryptoArgs().cryptoData(options["associatedData"], "associatedData",
                                                        true, false, true)
                     : checker_.cryptoArgs().cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return checker_.cryptoArgs().cryptoResult(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 3, 4);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> name = literalOf(args[0]);
    Expr *cipher = checker_.cryptoArgs().cryptoCipher(args[0]);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    Expr *iv = checker_.cryptoArgs().cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError() || !checker_.cryptoArgs().checkCipherLengths(cipher, key, iv)) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      checker_.diags_.error(args[0]->loc(), "'" + *name +
                                                "' needs the length of its tag: '{ "
                                                "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return checker_.cryptoArgs().cryptoResult(
        call, CryptoFn::CipherNew,
        {cipher, key, iv,
         tag ? checker_.cryptoArgs().cryptoNumber(tag)
             : checker_.cryptoArgs().cryptoDouble(loc, -1),
         checker_.cryptoArgs().cryptoBool(loc, decrypt)},
        Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 0, 0);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call,
                                            fn == "getHashes"    ? CryptoFn::GetHashes
                                            : fn == "getCiphers" ? CryptoFn::GetCiphers
                                                                 : CryptoFn::GetCurves,
                                            {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *key = checker_.cryptoArgs().cryptoData(args[0], "key", true, false, true);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = checker_.cryptoArgs().cryptoInt(loc, 0),
               *type = checker_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = checker_.cryptoArgs().cryptoUndefined(loc);
    int format = 0, has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!checker_.cryptoArgs().cryptoOptions(
              args[0], fn, {"key", "format", "type", "passphrase", "encoding"}, options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        if ((format = checker_.cryptoArgs().cryptoChoice(f, "format of the key",
                                                         {"pem", "der", "jwk"})) < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        checker_.diags_.error(args[0]->loc(), "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2
                ? checker_.argumentChecker().checkArg(options["key"], Type::json())
                : checker_.cryptoArgs().cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = checker_.cryptoArgs().cryptoChoice(t, "type of the key",
                                                       {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = checker_.cryptoArgs().cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = checker_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = literalOf(e) ? checker_.bytesChecker().checkBufferEncoding(e) : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          checker_.diags_.error(e->loc(),
                                "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = checker_.cryptoArgs().cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(
        call, CryptoFn::CreateKey,
        {checker_.cryptoArgs().cryptoBool(loc, priv), key, encoding,
         checker_.cryptoArgs().cryptoInt(loc, format), checker_.cryptoArgs().cryptoInt(loc, has),
         type, passphrase},
        keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return checker_.keyPairChecker().checkGenerateKeyPair(call, loc, shown);
  }
  if (fn == "generateKeySync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    int which = checker_.cryptoArgs().cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 || !checker_.cryptoArgs().cryptoOptions(args[1], shown, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      checker_.diags_.error(args[1]->loc(),
                            "'" + shown +
                                "' needs the length of the key in bits: '{ "
                                "length: 256 }'");
      return call;
    }
    Expr *bits = checker_.exprChecker().checkExpr(options["length"], Type::intTy());
    if (bits->type->isError()) {
      return call;
    }
    if (which == 1) {
      std::optional<int64_t> known = checker_.constantFolder().intConstant(bits);
      if (!known || (*known != 128 && *known != 192 && *known != 256)) {
        checker_.diags_.error(bits->loc(), "the length of an AES key is 128, 192 or 256, written "
                                           "as a number; JavaScript throws a TypeError otherwise");
        return call;
      }
    } else {
      IndexProofs::ResultBetweenProven between =
          checker_.indexProofs().requireBetween(bits, 8, {}, "the length of the key");
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        checker_.diags_.error(bits->loc(), between.error().message);
      }
      if (between.isErr() && !checker_.indexProofs().exempt(bits->loc())) {
        return call;
      }
    }
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::GenerateSecretKey,
        {checker_.cryptoArgs().cryptoBool(loc, which == 1),
         bits->type->isFloat() ? bits : checker_.typeRelations().convert(bits, Type::doubleTy())},
        keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    ArgumentChecker::ResultArityMatches arity =
        checker_.argumentChecker().checkArity(call, unsigned(count), unsigned(count + 1));
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || checker_.cryptoArgs().rejectCryptoCallback(
                             call, count, fn,
                             verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm = isa<UndefinedExpr>(args[0])
                          ? checker_.cryptoArgs().cryptoUndefined(loc)
                          : checker_.argumentChecker().checkArg(args[0], Type::string());
    Expr *data = checker_.cryptoArgs().cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!checker_.cryptoArgs().cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature = checker_.cryptoArgs().cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, verify ? CryptoFn::Verify : CryptoFn::Sign,
                                              std::move(signArgs),
                                              verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *name = args[0];
    bool literal = literalOf(name).has_value();
    if (literal && !checker_.cryptoArgs().cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    std::vector<Expr *> signArgs = {checker_.argumentChecker().checkArg(name, Type::string()),
                                    checker_.cryptoArgs().cryptoBool(loc, verify)};
    const Type *made = Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign);
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::SignNew, std::move(signArgs), made);
  }
  if (fn == "createECDH") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> curve = literalOf(args[0]);
    if (curve && !llvm::is_contained(kCurveNames, *curve)) {
      checker_.diags_.error(args[0]->loc(), "unknown curve '" + *curve +
                                                "'; node has those getCurves() lists, e.g. "
                                                "'prime256v1', 'secp384r1', 'secp256k1'");
      return loosely();
    }
    Expr *name = checker_.argumentChecker().checkArg(args[0], Type::string());
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::EcdhNew, {name},
                                              Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        !checker_.cryptoArgs().cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      checker_.diags_.error(args[0]->loc(), "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = checker_.cryptoArgs().cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = checker_.cryptoArgs().cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  if (fn == "generatePrimeSync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"add", "rem", "safe", "bigint"})) {
      return call;
    }
    if (Expr *big = options.lookup("bigint")) {
      checker_.diags_.error(big->loc(), "'bigint' is not supported: tz has no bigint "
                                        "(docs/numbers.md); the prime comes as an ArrayBuffer");
      return call;
    }
    Expr *size = checker_.cryptoArgs().cryptoNumber(args[0]);
    Expr *safe = options.lookup("safe")
                     ? checker_.argumentChecker().checkArg(options["safe"], Type::boolTy())
                     : checker_.cryptoArgs().cryptoBool(loc, false);
    Expr *add = options.lookup("add")
                    ? checker_.cryptoArgs().cryptoData(options["add"], "add", false, false, true)
                    : checker_.cryptoArgs().cryptoUndefined(loc);
    Expr *rem = options.lookup("rem")
                    ? checker_.cryptoArgs().cryptoData(options["rem"], "rem", false, false, true)
                    : checker_.cryptoArgs().cryptoUndefined(loc);
    if (size->type->isError() || safe->type->isError() || add->type->isError() ||
        rem->type->isError() || !checker_.cryptoArgs().checkWhole(size, "size", 1)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::GeneratePrime, {size, safe, add, rem},
                                              Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "checkPrimeSync") {
    ArgumentChecker::ResultArityMatches arity = checker_.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      checker_.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"checks"})) {
      return call;
    }
    Expr *candidate =
        checker_.cryptoArgs().cryptoData(args[0], "candidate", false, false, true);
    Expr *checks = options.lookup("checks") ? checker_.cryptoArgs().cryptoNumber(options["checks"])
                                            : checker_.cryptoArgs().cryptoDouble(loc, 0);
    if (candidate->type->isError() || checks->type->isError() ||
        !checker_.cryptoArgs().checkWhole(checks, "checks", 0)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoResult(call, CryptoFn::CheckPrime, {candidate, checks},
                                              Type::boolTy());
  }
  checker_.diags_.error(loc, cryptoExportProblem(shown, "node:crypto"));

  return loosely();
}

}
