#include "compiler/pipeline/check_program/topics/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args.h"
#include "compiler/pipeline/check_program/topics/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
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

CryptoFunctionChecker::CryptoFunctionChecker(CheckState &owner)
    : state(owner) {}

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
    this->state.getDiagnostics().error(
        call->args.back()->loc(),
        "'" + fn +
            "' with a callback is not supported: tz has no callbacks "
            "(docs/crypto.md, section 5); without one it returns a Promise: "
            "'const result = await " +
            fn + "(...)'");
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
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
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    this->state.getDiagnostics().error(
        loc, "'" + fn + "' of 'node:crypto' is a class, not a function" +
                 (fn == "X509Certificate" ? "; make one with 'X509Certificate.parse(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    this->state.getDiagnostics().error(
        loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
             "and the like");
    return loosely();
  }
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || CryptoArgs(this->state).cryptoOptions(args[i], shown, allowed, options);
  };

  if (fn == "createHash") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = CryptoArgs(this->state).cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    std::vector<Expr *> hashArgs = {md, length ? CryptoArgs(this->state).cryptoNumber(length)
                                               : CryptoArgs(this->state).cryptoDouble(loc, -1)};
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::HashNew, std::move(hashArgs), Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = CryptoArgs(this->state).cryptoMd(args[0], true);
    Expr *key = CryptoArgs(this->state).cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    std::vector<Expr *> hmacArgs = {md, key, CryptoArgs(this->state).cryptoInt(loc, 0)};
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::HmacNew, std::move(hmacArgs), Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = CryptoArgs(this->state).cryptoMd(args[0], false);
    Expr *data = CryptoArgs(this->state).cryptoData(args[1], "data", true, false, false);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 2, fn, 6);
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return CryptoArgs(this->state).cryptoResult(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::HashOnceText, {md, data, encoding}, Type::string());
  }
  auto proven = [&](Expr *arg, int64_t lo, std::optional<int64_t> hi,
                    llvm::StringRef what) -> Expr * {
    Expr *e = ExprChecker(this->state).checkExpr(arg, Type::intTy());
    if (e->type->isError()) {
      return nullptr;
    }
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state).requireBetween(e, lo, hi, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(e->loc(), between.error().message);
    }
    if (between.isErr() && !IndexProofs(this->state).exempt(e->loc())) {
      return nullptr;
    }
    return e->type->isFloat() ? e : TypeRelations(this->state).convert(e, Type::doubleTy());
  };
  if (fn == "randomBytes") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || CryptoArgs(this->state)
                             .rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    Expr *size = proven(args[0], 0, 2147483647, "the size");
    if (!size) {
      return call;
    }
    return CryptoArgs(this->state).cryptoCall(call, CryptoFn::RandomBytes, {size}, buffer);
  }
  if (fn == "randomUUID") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      ArgumentChecker(this->state).checkArg(cache, Type::boolTy());
    }
    return CryptoArgs(this->state).cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 &&
        CryptoArgs(this->state)
            .rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      this->state.getDiagnostics().error(args[2]->loc(),
                                         "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? ExprChecker(this->state).checkExpr(args[0], Type::intTy())
                                 : CryptoArgs(this->state).cryptoInt(loc, 0);
    Expr *max = ExprChecker(this->state).checkExpr(args.back(), Type::intTy());
    if (min->type->isError() || max->type->isError()) {
      return call;
    }
    for (Expr *bound : {min, max}) {
      if (!bound->type->isInt()) {
        this->state.getDiagnostics().error(bound->loc(),
                                           "'randomInt' takes whole numbers ('int'), found '" +
                                               bound->type->name() + "'");
        return call;
      }
    }
    IndexProofs::ResultLessWasProven less =
        IndexProofs(this->state)
            .requireLess(min, max,
                         args.size() == 2 ? "the maximum is greater than the minimum"
                                          : "the maximum is greater than 0");
    if (less.isErr()) {
      this->state.getDiagnostics().error(max->loc(), less.error().message);
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, 1, fn == "randomFillSync" ? 3 : 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *target = ExprChecker(this->state).checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        this->state.getDiagnostics().error(
            target->loc(), "'getRandomValues' fills an array of integers (a Uint8Array, "
                           "an Int32Array, a Buffer...); found '" +
                               type->name() + "'");
        return call;
      }
      return CryptoArgs(this->state).cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      this->state.getDiagnostics().error(target->loc(),
                                         "'" + shown +
                                             "' fills bytes (a Buffer, a typed array, a "
                                             "DataView or an ArrayBuffer); found '" +
                                             type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = CryptoArgs(this->state).cryptoDouble(loc, 0);
    Expr *size = CryptoArgs(this->state).cryptoDouble(loc, 0);
    if (present(1)) {
      Expr *at = ExprChecker(this->state).checkExpr(args[1], Type::intTy());
      bool library = IndexProofs(this->state).exempt(at->loc());
      if (at->type->isError()) {
        return call;
      }
      if (!library) {
        IndexProofs::ResultBetweenProven between =
            IndexProofs(this->state).requireBetween(at, 0, {}, "the offset");
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(at->loc(), between.error().message);
        }
        if (between.isErr()) {
          return call;
        }
        IndexProofs::ResultUpToWasProven upTo =
            IndexProofs(this->state).requireUpTo(at, target, 0, "the offset");
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(at->loc(), upTo.error().message);
        }
        if (upTo.isErr()) {
          return call;
        }
      }
      Expr *count = nullptr;
      if (present(2)) {
        count = ExprChecker(this->state).checkExpr(args[2], Type::intTy());
        if (count->type->isError()) {
          return call;
        }
        if (!library) {
          IndexProofs::ResultBetweenProven sizeBetween =
              IndexProofs(this->state).requireBetween(count, 0, {}, "the size");
          if (sizeBetween.isErr() &&
              sizeBetween.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
            this->state.getDiagnostics().error(count->loc(), sizeBetween.error().message);
          }
          if (sizeBetween.isErr()) {
            return call;
          }
        }
        if (!IndexProofs(this->state)
                 .provesSumUpTo(IndexProofs(this->state).linear(at),
                                IndexProofs(this->state).linear(count), target) &&
            !IndexProofs(this->state).exempt(count->loc())) {
          this->state.getDiagnostics().error(
              count->loc(), "cannot prove that the offset and the size fit in the bytes, "
                            "and JavaScript would throw a RangeError; check it first, e.g. "
                            "'if (" +
                                valueText(count) + " <= buf.length - " + valueText(at) + ")'");
          return call;
        }
        size = count->type->isFloat() ? count
                                      : TypeRelations(this->state).convert(count, Type::doubleTy());
      }
      offset = at->type->isFloat() ? at : TypeRelations(this->state).convert(at, Type::doubleTy());
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::RandomFill,
                    {target, offset, size, CryptoArgs(this->state).cryptoInt(loc, has)}, type);
  }
  if (fn == "timingSafeEqual") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *a = CryptoArgs(this->state).cryptoData(args[0], "first argument", false, false, true);
    Expr *b = CryptoArgs(this->state).cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b}, Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *password = CryptoArgs(this->state).cryptoData(args[0], "password", true, true, true);
    Expr *salt = CryptoArgs(this->state).cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = CryptoArgs(this->state).cryptoNumber(args[2]),
         *keylen = CryptoArgs(this->state).cryptoNumber(args[3]);
    Expr *md = CryptoArgs(this->state).cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError() ||
        !CryptoArgs(this->state).checkWhole(iterations, "iterations", 1) ||
        !CryptoArgs(this->state).checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::Pbkdf2, {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = CryptoArgs(this->state).cryptoMd(args[0], true);
    Expr *key = CryptoArgs(this->state).cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = CryptoArgs(this->state).cryptoData(args[2], "salt", true, false, true);
    Expr *info = CryptoArgs(this->state).cryptoData(args[3], "info", true, false, true);
    Expr *keylen = CryptoArgs(this->state).cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError() ||
        !CryptoArgs(this->state).checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                      Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *password = CryptoArgs(this->state).cryptoData(args[0], "password", true, true, true);
    Expr *salt = CryptoArgs(this->state).cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = CryptoArgs(this->state).cryptoNumber(args[2]);
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
        this->state.getDiagnostics().error(
            b->loc(), "option '" + pairs[i].first + "' cannot be used together with '" +
                          pairs[i].second + "': they are one option");
        return call;
      }
      values[i] = a || b ? CryptoArgs(this->state).cryptoNumber(a ? a : b)
                         : CryptoArgs(this->state).cryptoDouble(loc, 0);
    }
    if (std::optional<double> cost = CryptoArgs(this->state).knownNumber(values[0])) {
      int64_t n = int64_t(*cost);
      if (*cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0)) {
        this->state.getDiagnostics().error(values[0]->loc(),
                                           "N must be a power of two greater than 1, not " +
                                               CryptoArgs::numberText(*cost));
        return call;
      }
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::Scrypt,
                      {password, salt, keylen, values[0], values[1], values[2],
                       maxmem ? CryptoArgs(this->state).cryptoNumber(maxmem)
                              : CryptoArgs(this->state).cryptoDouble(loc, 0)},
                      buffer);
  }
  if (fn == "argon2Sync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *type, *name;
    if (CryptoFunctionChecker::literalOf(args[0])) {
      int which =
          CryptoArgs(this->state)
              .cryptoChoice(args[0], "algorithm of Argon2", {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = CryptoArgs(this->state).cryptoInt(loc, which);
      name = CryptoArgs(this->state).cryptoString(loc, "");
    } else {
      type = CryptoArgs(this->state).cryptoInt(loc, 0xffffffff);
      name = ArgumentChecker(this->state).checkArg(args[0], Type::string());
    }
    if (!CryptoArgs(this->state)
             .cryptoOptions(args[1], shown,
                            {"message", "nonce", "parallelism", "tagLength", "memory", "passes",
                             "secret", "associatedData"},
                            options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        this->state.getDiagnostics().error(args[1]->loc(), "the parameters of '" + shown +
                                                               "' need '" + required + "'");
        return call;
      }
    }
    Expr *message =
        CryptoArgs(this->state).cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = CryptoArgs(this->state).cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = CryptoArgs(this->state).cryptoNumber(options["parallelism"]);
    Expr *tagLength = CryptoArgs(this->state).cryptoNumber(options["tagLength"]);
    Expr *memory = CryptoArgs(this->state).cryptoNumber(options["memory"]);
    Expr *passes = CryptoArgs(this->state).cryptoNumber(options["passes"]);
    Expr *secret =
        options.lookup("secret")
            ? CryptoArgs(this->state).cryptoData(options["secret"], "secret", true, false, true)
            : CryptoArgs(this->state).cryptoUndefined(loc);
    Expr *data =
        options.lookup("associatedData")
            ? CryptoArgs(this->state)
                  .cryptoData(options["associatedData"], "associatedData", true, false, true)
            : CryptoArgs(this->state).cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return CryptoArgs(this->state)
        .cryptoResult(
            call, CryptoFn::Argon2,
            {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data},
            buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> name = CryptoFunctionChecker::literalOf(args[0]);
    Expr *cipher = CryptoArgs(this->state).cryptoCipher(args[0]);
    Expr *key = CryptoArgs(this->state).cryptoData(args[1], "key", true, true, true);
    Expr *iv = CryptoArgs(this->state).cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError() || !CryptoArgs(this->state).checkCipherLengths(cipher, key, iv)) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      this->state.getDiagnostics().error(args[0]->loc(), "'" + *name +
                                                             "' needs the length of its tag: '{ "
                                                             "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::CipherNew,
                      {cipher, key, iv,
                       tag ? CryptoArgs(this->state).cryptoNumber(tag)
                           : CryptoArgs(this->state).cryptoDouble(loc, -1),
                       CryptoArgs(this->state).cryptoBool(loc, decrypt)},
                      Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoCall(call,
                    fn == "getHashes"    ? CryptoFn::GetHashes
                    : fn == "getCiphers" ? CryptoFn::GetCiphers
                                         : CryptoFn::GetCurves,
                    {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *key = CryptoArgs(this->state).cryptoData(args[0], "key", true, false, true);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = CryptoArgs(this->state).cryptoInt(loc, 0),
               *type = CryptoArgs(this->state).cryptoString(loc, "");
    Expr *passphrase = CryptoArgs(this->state).cryptoUndefined(loc);
    int format = 0;
    int has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!CryptoArgs(this->state)
               .cryptoOptions(args[0], fn, {"key", "format", "type", "passphrase", "encoding"},
                              options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        if ((format = CryptoArgs(this->state)
                          .cryptoChoice(f, "format of the key", {"pem", "der", "jwk"})) < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        this->state.getDiagnostics().error(args[0]->loc(),
                                           "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2
                ? ArgumentChecker(this->state).checkArg(options["key"], Type::json())
                : CryptoArgs(this->state).cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = CryptoArgs(this->state)
                        .cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = CryptoArgs(this->state).cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = CryptoArgs(this->state).cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = CryptoFunctionChecker::literalOf(e)
                           ? BytesChecker(this->state).checkBufferEncoding(e)
                           : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          this->state.getDiagnostics().error(
              e->loc(), "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = CryptoArgs(this->state).cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::CreateKey,
                      {CryptoArgs(this->state).cryptoBool(loc, priv), key, encoding,
                       CryptoArgs(this->state).cryptoInt(loc, format),
                       CryptoArgs(this->state).cryptoInt(loc, has), type, passphrase},
                      keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return KeyPairChecker(this->state).checkGenerateKeyPair(call, loc, shown);
  }
  if (fn == "generateKeySync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    int which = CryptoArgs(this->state).cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 || !CryptoArgs(this->state).cryptoOptions(args[1], shown, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      this->state.getDiagnostics().error(args[1]->loc(),
                                         "'" + shown +
                                             "' needs the length of the key in bits: '{ "
                                             "length: 256 }'");
      return call;
    }
    Expr *bits = ExprChecker(this->state).checkExpr(options["length"], Type::intTy());
    if (bits->type->isError()) {
      return call;
    }
    if (which == 1) {
      std::optional<int64_t> known = ConstantFolder(this->state).intConstant(bits);
      if (!known || (*known != 128 && *known != 192 && *known != 256)) {
        this->state.getDiagnostics().error(bits->loc(),
                                           "the length of an AES key is 128, 192 or 256, written "
                                           "as a number; JavaScript throws a TypeError otherwise");
        return call;
      }
    } else {
      IndexProofs::ResultBetweenProven between =
          IndexProofs(this->state).requireBetween(bits, 8, {}, "the length of the key");
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(bits->loc(), between.error().message);
      }
      if (between.isErr() && !IndexProofs(this->state).exempt(bits->loc())) {
        return call;
      }
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::GenerateSecretKey,
                    {CryptoArgs(this->state).cryptoBool(loc, which == 1),
                     bits->type->isFloat()
                         ? bits
                         : TypeRelations(this->state).convert(bits, Type::doubleTy())},
                    keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, unsigned(count), unsigned(count + 1));
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        CryptoArgs(this->state)
            .rejectCryptoCallback(call, count, fn,
                                  verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                         : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm = isa<UndefinedExpr>(args[0])
                          ? CryptoArgs(this->state).cryptoUndefined(loc)
                          : ArgumentChecker(this->state).checkArg(args[0], Type::string());
    Expr *data = CryptoArgs(this->state).cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!CryptoArgs(this->state).cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature = CryptoArgs(this->state).cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, verify ? CryptoFn::Verify : CryptoFn::Sign, std::move(signArgs),
                      verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *name = args[0];
    bool literal = CryptoFunctionChecker::literalOf(name).has_value();
    if (literal && !CryptoArgs(this->state).cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    std::vector<Expr *> signArgs = {ArgumentChecker(this->state).checkArg(name, Type::string()),
                                    CryptoArgs(this->state).cryptoBool(loc, verify)};
    const Type *made = Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign);
    return CryptoArgs(this->state).cryptoResult(call, CryptoFn::SignNew, std::move(signArgs), made);
  }
  if (fn == "createECDH") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> curve = CryptoFunctionChecker::literalOf(args[0]);
    if (curve && !llvm::is_contained(kCurveNames, *curve)) {
      this->state.getDiagnostics().error(args[0]->loc(),
                                         "unknown curve '" + *curve +
                                             "'; node has those getCurves() lists, e.g. "
                                             "'prime256v1', 'secp384r1', 'secp256k1'");
      return loosely();
    }
    Expr *name = ArgumentChecker(this->state).checkArg(args[0], Type::string());
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::EcdhNew, {name}, Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        !CryptoArgs(this->state).cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      this->state.getDiagnostics().error(args[0]->loc(),
                                         "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = CryptoArgs(this->state).cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = CryptoArgs(this->state).cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state).cryptoResult(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  if (fn == "generatePrimeSync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"add", "rem", "safe", "bigint"})) {
      return call;
    }
    if (Expr *big = options.lookup("bigint")) {
      this->state.getDiagnostics().error(big->loc(),
                                         "'bigint' is not supported: tz has no bigint "
                                         "(docs/numbers.md); the prime comes as an ArrayBuffer");
      return call;
    }
    Expr *size = CryptoArgs(this->state).cryptoNumber(args[0]);
    Expr *safe = options.lookup("safe")
                     ? ArgumentChecker(this->state).checkArg(options["safe"], Type::boolTy())
                     : CryptoArgs(this->state).cryptoBool(loc, false);
    Expr *add = options.lookup("add")
                    ? CryptoArgs(this->state).cryptoData(options["add"], "add", false, false, true)
                    : CryptoArgs(this->state).cryptoUndefined(loc);
    Expr *rem = options.lookup("rem")
                    ? CryptoArgs(this->state).cryptoData(options["rem"], "rem", false, false, true)
                    : CryptoArgs(this->state).cryptoUndefined(loc);
    if (size->type->isError() || safe->type->isError() || add->type->isError() ||
        rem->type->isError() || !CryptoArgs(this->state).checkWhole(size, "size", 1)) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::GeneratePrime, {size, safe, add, rem},
                      Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "checkPrimeSync") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"checks"})) {
      return call;
    }
    Expr *candidate = CryptoArgs(this->state).cryptoData(args[0], "candidate", false, false, true);
    Expr *checks = options.lookup("checks")
                       ? CryptoArgs(this->state).cryptoNumber(options["checks"])
                       : CryptoArgs(this->state).cryptoDouble(loc, 0);
    if (candidate->type->isError() || checks->type->isError() ||
        !CryptoArgs(this->state).checkWhole(checks, "checks", 0)) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::CheckPrime, {candidate, checks}, Type::boolTy());
  }
  this->state.getDiagnostics().error(loc, cryptoExportProblem(shown, "node:crypto"));

  return loosely();
}

}
