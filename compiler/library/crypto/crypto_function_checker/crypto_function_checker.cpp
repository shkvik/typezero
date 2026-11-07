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
using llvm::isa;

namespace tz {

std::string CryptoFunctionChecker::utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::string> CryptoFunctionChecker::literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return CryptoFunctionChecker::utf8Of(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return CryptoFunctionChecker::utf8Of(tmpl->quasis[0]);
  }

  return std::nullopt;
}

CryptoFunctionChecker::CryptoFunctionChecker(Checker &owner)
    : checker(owner) {}

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
    this->checker.diags_.error(call->args.back()->loc(),
                               "'" + fn +
                                   "' with a callback is not supported: tz has no callbacks "
                                   "(docs/crypto.md, section 5); without one it returns a Promise: "
                                   "'const result = await " +
                                   fn + "(...)'");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  Expr *checked = this->checkSyncCall(call, sync, fn, loc);

  return async ? this->asyncOf(checked) : checked;
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
      arg = this->checker.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    this->checker.diags_.error(
        loc, "'" + fn + "' of 'node:crypto' is a class, not a function" +
                 (fn == "X509Certificate" ? "; make one with 'X509Certificate.parse(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    this->checker.diags_.error(
        loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
             "and the like");
    return loosely();
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) ||
           this->checker.cryptoArgs().cryptoOptions(args[i], shown, allowed, options);
  };

  if (fn == "createHash") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->checker.cryptoArgs().cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    std::vector<Expr *> hashArgs = {md, length ? this->checker.cryptoArgs().cryptoNumber(length)
                                               : this->checker.cryptoArgs().cryptoDouble(loc, -1)};
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::HashNew, std::move(hashArgs),
                                                   Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->checker.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = this->checker.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    std::vector<Expr *> hmacArgs = {md, key, this->checker.cryptoArgs().cryptoInt(loc, 0)};
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::HmacNew, std::move(hmacArgs),
                                                   Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->checker.cryptoArgs().cryptoMd(args[0], false);
    Expr *data = this->checker.cryptoArgs().cryptoData(args[1], "data", true, false, false);
    Expr *encoding = this->checker.cryptoArgs().cryptoEncoding(call, 2, fn, 6);
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::HashOnceText,
                                                   {md, data, encoding}, Type::string());
  }
  auto proven = [&](Expr *arg, int64_t lo, std::optional<int64_t> hi,
                    llvm::StringRef what) -> Expr * {
    Expr *e = this->checker.exprChecker().checkExpr(arg, Type::intTy());
    if (e->type->isError()) {
      return nullptr;
    }
    IndexProofs::ResultBetweenProven between =
        this->checker.indexProofs().requireBetween(e, lo, hi, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->checker.diags_.error(e->loc(), between.error().message);
    }
    if (between.isErr() && !this->checker.indexProofs().exempt(e->loc())) {
      return nullptr;
    }
    return e->type->isFloat() ? e : this->checker.typeRelations().convert(e, Type::doubleTy());
  };
  if (fn == "randomBytes") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || this->checker.cryptoArgs().rejectCryptoCallback(
                             call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    Expr *size = proven(args[0], 0, 2147483647, "the size");
    if (!size) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::RandomBytes, {size}, buffer);
  }
  if (fn == "randomUUID") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      this->checker.argumentChecker().checkArg(cache, Type::boolTy());
    }
    return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 && this->checker.cryptoArgs().rejectCryptoCallback(
                               call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      this->checker.diags_.error(args[2]->loc(), "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? this->checker.exprChecker().checkExpr(args[0], Type::intTy())
                                 : this->checker.cryptoArgs().cryptoInt(loc, 0);
    Expr *max = this->checker.exprChecker().checkExpr(args.back(), Type::intTy());
    if (min->type->isError() || max->type->isError()) {
      return call;
    }
    for (Expr *bound : {min, max}) {
      if (!bound->type->isInt()) {
        this->checker.diags_.error(bound->loc(),
                                   "'randomInt' takes whole numbers ('int'), found '" +
                                       bound->type->name() + "'");
        return call;
      }
    }
    IndexProofs::ResultLessWasProven less = this->checker.indexProofs().requireLess(
        min, max,
        args.size() == 2 ? "the maximum is greater than the minimum"
                         : "the maximum is greater than 0");
    if (less.isErr()) {
      this->checker.diags_.error(max->loc(), less.error().message);
      return call;
    }
    return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::RandomInt32, {min, max},
                                                 Type::intTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, fn == "randomFillSync" ? 3 : 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *target = this->checker.exprChecker().checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        this->checker.diags_.error(target->loc(),
                                   "'getRandomValues' fills an array of integers (a Uint8Array, "
                                   "an Int32Array, a Buffer...); found '" +
                                       type->name() + "'");
        return call;
      }
      return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      this->checker.diags_.error(target->loc(), "'" + shown +
                                                    "' fills bytes (a Buffer, a typed array, a "
                                                    "DataView or an ArrayBuffer); found '" +
                                                    type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = this->checker.cryptoArgs().cryptoDouble(loc, 0);
    Expr *size = this->checker.cryptoArgs().cryptoDouble(loc, 0);
    if (present(1)) {
      Expr *at = this->checker.exprChecker().checkExpr(args[1], Type::intTy());
      bool library = this->checker.indexProofs().exempt(at->loc());
      if (at->type->isError()) {
        return call;
      }
      if (!library) {
        IndexProofs::ResultBetweenProven between =
            this->checker.indexProofs().requireBetween(at, 0, {}, "the offset");
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->checker.diags_.error(at->loc(), between.error().message);
        }
        if (between.isErr()) {
          return call;
        }
        IndexProofs::ResultUpToWasProven upTo =
            this->checker.indexProofs().requireUpTo(at, target, 0, "the offset");
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->checker.diags_.error(at->loc(), upTo.error().message);
        }
        if (upTo.isErr()) {
          return call;
        }
      }
      Expr *count = nullptr;
      if (present(2)) {
        count = this->checker.exprChecker().checkExpr(args[2], Type::intTy());
        if (count->type->isError()) {
          return call;
        }
        if (!library) {
          IndexProofs::ResultBetweenProven sizeBetween =
              this->checker.indexProofs().requireBetween(count, 0, {}, "the size");
          if (sizeBetween.isErr() &&
              sizeBetween.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
            this->checker.diags_.error(count->loc(), sizeBetween.error().message);
          }
          if (sizeBetween.isErr()) {
            return call;
          }
        }
        if (!this->checker.indexProofs().provesSumUpTo(this->checker.indexProofs().linear(at),
                                                       this->checker.indexProofs().linear(count),
                                                       target) &&
            !this->checker.indexProofs().exempt(count->loc())) {
          this->checker.diags_.error(
              count->loc(), "cannot prove that the offset and the size fit in the bytes, "
                            "and JavaScript would throw a RangeError; check it first, e.g. "
                            "'if (" +
                                valueText(count) + " <= buf.length - " + valueText(at) + ")'");
          return call;
        }
        size = count->type->isFloat()
                   ? count
                   : this->checker.typeRelations().convert(count, Type::doubleTy());
      }
      offset =
          at->type->isFloat() ? at : this->checker.typeRelations().convert(at, Type::doubleTy());
    }
    return this->checker.cryptoArgs().cryptoCall(
        call, CryptoFn::RandomFill,
        {target, offset, size, this->checker.cryptoArgs().cryptoInt(loc, has)}, type);
  }
  if (fn == "timingSafeEqual") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *a = this->checker.cryptoArgs().cryptoData(args[0], "first argument", false, false, true);
    Expr *b = this->checker.cryptoArgs().cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b},
                                                 Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *password = this->checker.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = this->checker.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = this->checker.cryptoArgs().cryptoNumber(args[2]),
         *keylen = this->checker.cryptoArgs().cryptoNumber(args[3]);
    Expr *md = this->checker.cryptoArgs().cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError() ||
        !this->checker.cryptoArgs().checkWhole(iterations, "iterations", 1) ||
        !this->checker.cryptoArgs().checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(
        call, CryptoFn::Pbkdf2, {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->checker.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = this->checker.cryptoArgs().cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = this->checker.cryptoArgs().cryptoData(args[2], "salt", true, false, true);
    Expr *info = this->checker.cryptoArgs().cryptoData(args[3], "info", true, false, true);
    Expr *keylen = this->checker.cryptoArgs().cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError() ||
        !this->checker.cryptoArgs().checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::Hkdf,
                                                   {md, key, salt, info, keylen},
                                                   Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *password = this->checker.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = this->checker.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = this->checker.cryptoArgs().cryptoNumber(args[2]);
    if (!optionsAt(3, {"N", "r", "p", "maxmem", "cost", "blockSize", "parallelization"})) {
      return call;
    }
    static const std::pair<llvm::StringLiteral, llvm::StringLiteral> pairs[] = {
        {"N", "cost"}, {"r", "blockSize"}, {"p", "parallelization"}};
    Expr *values[3];
    for (int i = 0; i < 3; ++i) {
      Expr *a = options.lookup(pairs[i].first);
      Expr *b = options.lookup(pairs[i].second);
      if (a && b) {
        this->checker.diags_.error(b->loc(), "option '" + pairs[i].first +
                                                 "' cannot be used together with '" +
                                                 pairs[i].second + "': they are one option");
        return call;
      }
      values[i] = a || b ? this->checker.cryptoArgs().cryptoNumber(a ? a : b)
                         : this->checker.cryptoArgs().cryptoDouble(loc, 0);
    }
    if (std::optional<double> cost = this->checker.cryptoArgs().knownNumber(values[0])) {
      int64_t n = int64_t(*cost);
      if (*cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0)) {
        this->checker.diags_.error(values[0]->loc(),
                                   "N must be a power of two greater than 1, not " +
                                       CryptoArgs::numberText(*cost));
        return call;
      }
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(
        call, CryptoFn::Scrypt,
        {password, salt, keylen, values[0], values[1], values[2],
         maxmem ? this->checker.cryptoArgs().cryptoNumber(maxmem)
                : this->checker.cryptoArgs().cryptoDouble(loc, 0)},
        buffer);
  }
  if (fn == "argon2Sync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *type, *name;
    if (CryptoFunctionChecker::literalOf(args[0])) {
      int which = this->checker.cryptoArgs().cryptoChoice(args[0], "algorithm of Argon2",
                                                          {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = this->checker.cryptoArgs().cryptoInt(loc, which);
      name = this->checker.cryptoArgs().cryptoString(loc, "");
    } else {
      type = this->checker.cryptoArgs().cryptoInt(loc, 0xffffffff);
      name = this->checker.argumentChecker().checkArg(args[0], Type::string());
    }
    if (!this->checker.cryptoArgs().cryptoOptions(args[1], shown,
                                                  {"message", "nonce", "parallelism", "tagLength",
                                                   "memory", "passes", "secret", "associatedData"},
                                                  options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        this->checker.diags_.error(args[1]->loc(),
                                   "the parameters of '" + shown + "' need '" + required + "'");
        return call;
      }
    }
    Expr *message =
        this->checker.cryptoArgs().cryptoData(options["message"], "message", true, false, true);
    Expr *nonce =
        this->checker.cryptoArgs().cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = this->checker.cryptoArgs().cryptoNumber(options["parallelism"]);
    Expr *tagLength = this->checker.cryptoArgs().cryptoNumber(options["tagLength"]);
    Expr *memory = this->checker.cryptoArgs().cryptoNumber(options["memory"]);
    Expr *passes = this->checker.cryptoArgs().cryptoNumber(options["passes"]);
    Expr *secret =
        options.lookup("secret")
            ? this->checker.cryptoArgs().cryptoData(options["secret"], "secret", true, false, true)
            : this->checker.cryptoArgs().cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? this->checker.cryptoArgs().cryptoData(options["associatedData"],
                                                             "associatedData", true, false, true)
                     : this->checker.cryptoArgs().cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return this->checker.cryptoArgs().cryptoResult(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> name = CryptoFunctionChecker::literalOf(args[0]);
    Expr *cipher = this->checker.cryptoArgs().cryptoCipher(args[0]);
    Expr *key = this->checker.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    Expr *iv = this->checker.cryptoArgs().cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError() || !this->checker.cryptoArgs().checkCipherLengths(cipher, key, iv)) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      this->checker.diags_.error(args[0]->loc(), "'" + *name +
                                                     "' needs the length of its tag: '{ "
                                                     "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return this->checker.cryptoArgs().cryptoResult(
        call, CryptoFn::CipherNew,
        {cipher, key, iv,
         tag ? this->checker.cryptoArgs().cryptoNumber(tag)
             : this->checker.cryptoArgs().cryptoDouble(loc, -1),
         this->checker.cryptoArgs().cryptoBool(loc, decrypt)},
        Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    return this->checker.cryptoArgs().cryptoCall(call,
                                                 fn == "getHashes"    ? CryptoFn::GetHashes
                                                 : fn == "getCiphers" ? CryptoFn::GetCiphers
                                                                      : CryptoFn::GetCurves,
                                                 {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *key = this->checker.cryptoArgs().cryptoData(args[0], "key", true, false, true);
    Expr *encoding = this->checker.cryptoArgs().cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->checker.cryptoArgs().cryptoInt(loc, 0);
    }
    return this->checker.cryptoArgs().cryptoCall(call, CryptoFn::SecretKey, {key, encoding},
                                                 keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = this->checker.cryptoArgs().cryptoInt(loc, 0),
               *type = this->checker.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = this->checker.cryptoArgs().cryptoUndefined(loc);
    int format = 0;
    int has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!this->checker.cryptoArgs().cryptoOptions(
              args[0], fn, {"key", "format", "type", "passphrase", "encoding"}, options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        if ((format = this->checker.cryptoArgs().cryptoChoice(f, "format of the key",
                                                              {"pem", "der", "jwk"})) < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        this->checker.diags_.error(args[0]->loc(), "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2
                ? this->checker.argumentChecker().checkArg(options["key"], Type::json())
                : this->checker.cryptoArgs().cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = this->checker.cryptoArgs().cryptoChoice(t, "type of the key",
                                                            {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = this->checker.cryptoArgs().cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = this->checker.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = CryptoFunctionChecker::literalOf(e)
                           ? this->checker.bytesChecker().checkBufferEncoding(e)
                           : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          this->checker.diags_.error(e->loc(),
                                     "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = this->checker.cryptoArgs().cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(
        call, CryptoFn::CreateKey,
        {this->checker.cryptoArgs().cryptoBool(loc, priv), key, encoding,
         this->checker.cryptoArgs().cryptoInt(loc, format),
         this->checker.cryptoArgs().cryptoInt(loc, has), type, passphrase},
        keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return this->checker.keyPairChecker().checkGenerateKeyPair(call, loc, shown);
  }
  if (fn == "generateKeySync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    int which =
        this->checker.cryptoArgs().cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 ||
        !this->checker.cryptoArgs().cryptoOptions(args[1], shown, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      this->checker.diags_.error(args[1]->loc(), "'" + shown +
                                                     "' needs the length of the key in bits: '{ "
                                                     "length: 256 }'");
      return call;
    }
    Expr *bits = this->checker.exprChecker().checkExpr(options["length"], Type::intTy());
    if (bits->type->isError()) {
      return call;
    }
    if (which == 1) {
      std::optional<int64_t> known = this->checker.constantFolder().intConstant(bits);
      if (!known || (*known != 128 && *known != 192 && *known != 256)) {
        this->checker.diags_.error(bits->loc(),
                                   "the length of an AES key is 128, 192 or 256, written "
                                   "as a number; JavaScript throws a TypeError otherwise");
        return call;
      }
    } else {
      IndexProofs::ResultBetweenProven between =
          this->checker.indexProofs().requireBetween(bits, 8, {}, "the length of the key");
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->checker.diags_.error(bits->loc(), between.error().message);
      }
      if (between.isErr() && !this->checker.indexProofs().exempt(bits->loc())) {
        return call;
      }
    }
    return this->checker.cryptoArgs().cryptoCall(
        call, CryptoFn::GenerateSecretKey,
        {this->checker.cryptoArgs().cryptoBool(loc, which == 1),
         bits->type->isFloat() ? bits
                               : this->checker.typeRelations().convert(bits, Type::doubleTy())},
        keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, unsigned(count), unsigned(count + 1));
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || this->checker.cryptoArgs().rejectCryptoCallback(
                             call, count, fn,
                             verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm = isa<UndefinedExpr>(args[0])
                          ? this->checker.cryptoArgs().cryptoUndefined(loc)
                          : this->checker.argumentChecker().checkArg(args[0], Type::string());
    Expr *data = this->checker.cryptoArgs().cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!this->checker.cryptoArgs().cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature =
          this->checker.cryptoArgs().cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(call, verify ? CryptoFn::Verify : CryptoFn::Sign,
                                                   std::move(signArgs),
                                                   verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    Expr *name = args[0];
    bool literal = CryptoFunctionChecker::literalOf(name).has_value();
    if (literal && !this->checker.cryptoArgs().cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    std::vector<Expr *> signArgs = {this->checker.argumentChecker().checkArg(name, Type::string()),
                                    this->checker.cryptoArgs().cryptoBool(loc, verify)};
    const Type *made = Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign);
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::SignNew, std::move(signArgs),
                                                   made);
  }
  if (fn == "createECDH") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> curve = CryptoFunctionChecker::literalOf(args[0]);
    if (curve && !llvm::is_contained(kCurveNames, *curve)) {
      this->checker.diags_.error(args[0]->loc(), "unknown curve '" + *curve +
                                                     "'; node has those getCurves() lists, e.g. "
                                                     "'prime256v1', 'secp384r1', 'secp256k1'");
      return loosely();
    }
    Expr *name = this->checker.argumentChecker().checkArg(args[0], Type::string());
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::EcdhNew, {name},
                                                   Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !this->checker.cryptoArgs().cryptoOptions(
                             args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      this->checker.diags_.error(args[0]->loc(),
                                 "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = this->checker.cryptoArgs().cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = this->checker.cryptoArgs().cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::DiffieHellman, {priv, pub},
                                                   buffer);
  }
  if (fn == "generatePrimeSync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"add", "rem", "safe", "bigint"})) {
      return call;
    }
    if (Expr *big = options.lookup("bigint")) {
      this->checker.diags_.error(big->loc(),
                                 "'bigint' is not supported: tz has no bigint "
                                 "(docs/numbers.md); the prime comes as an ArrayBuffer");
      return call;
    }
    Expr *size = this->checker.cryptoArgs().cryptoNumber(args[0]);
    Expr *safe = options.lookup("safe")
                     ? this->checker.argumentChecker().checkArg(options["safe"], Type::boolTy())
                     : this->checker.cryptoArgs().cryptoBool(loc, false);
    Expr *add = options.lookup("add") ? this->checker.cryptoArgs().cryptoData(options["add"], "add",
                                                                              false, false, true)
                                      : this->checker.cryptoArgs().cryptoUndefined(loc);
    Expr *rem = options.lookup("rem") ? this->checker.cryptoArgs().cryptoData(options["rem"], "rem",
                                                                              false, false, true)
                                      : this->checker.cryptoArgs().cryptoUndefined(loc);
    if (size->type->isError() || safe->type->isError() || add->type->isError() ||
        rem->type->isError() || !this->checker.cryptoArgs().checkWhole(size, "size", 1)) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::GeneratePrime,
                                                   {size, safe, add, rem},
                                                   Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "checkPrimeSync") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"checks"})) {
      return call;
    }
    Expr *candidate =
        this->checker.cryptoArgs().cryptoData(args[0], "candidate", false, false, true);
    Expr *checks = options.lookup("checks")
                       ? this->checker.cryptoArgs().cryptoNumber(options["checks"])
                       : this->checker.cryptoArgs().cryptoDouble(loc, 0);
    if (candidate->type->isError() || checks->type->isError() ||
        !this->checker.cryptoArgs().checkWhole(checks, "checks", 0)) {
      return call;
    }
    return this->checker.cryptoArgs().cryptoResult(call, CryptoFn::CheckPrime, {candidate, checks},
                                                   Type::boolTy());
  }
  this->checker.diags_.error(loc, cryptoExportProblem(shown, "node:crypto"));

  return loosely();
}

}
