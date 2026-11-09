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
  this->cryptoArgs = &all.getCryptoArgs();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->indexProofs = &all.getIndexProofs();
  this->bytesChecker = &all.getBytesChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
  this->keyPairChecker = &all.getKeyPairChecker();
  this->argumentChecker = &all.getArgumentChecker();
}

std::optional<std::string> CryptoFunctionChecker::literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return ToUtf8::toUtf8(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return ToUtf8::toUtf8(tmpl->quasis[0]);
  }

  return std::nullopt;
}

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
    std::string message = ErrorTextCryptoFunction::callback(fn);
    this->state.getDiagnostics().error(call->args.back()->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    call->type = Type::error();
    return call;
  }
  Expr *checked = this->checkSyncCall(call, sync, fn, loc);

  return async ? this->asyncOf(checked) : checked;
}

CryptoFunctionChecker::ResultFilledTarget
CryptoFunctionChecker::filledTarget(const Type *type, llvm::StringRef fn, llvm::StringRef shown) {
  if (fn == "getRandomValues") {
    static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                         Type::Bytes::Float64};
    if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
      std::string message = ErrorTextCryptoFunction::randomVals(type->name());

      return err(CryptoFunctionCheckerFilledTargetError{
          .code = CryptoFunctionCheckerFilledTargetErrorCode::NOT_INTEGERS,
          .message = message,
      });
    }

    return ok(type);
  }
  if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
    std::string message = ErrorTextCryptoFunction::fillsBytes(shown, type->name());

    return err(CryptoFunctionCheckerFilledTargetError{
        .code = CryptoFunctionCheckerFilledTargetErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

CryptoFunctionChecker::ResultCurveIsKnown
CryptoFunctionChecker::curveIsKnown(llvm::StringRef curve) {
  if (!llvm::is_contained(kCurveNames, curve)) {
    std::string message = ErrorTextCryptoFunction::curve(curve);

    return err(CryptoFunctionCheckerCurveIsKnownError{
        .code = CryptoFunctionCheckerCurveIsKnownErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
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
      arg = this->exprChecker->checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    std::string message = ErrorTextCryptoFunction::isClass(fn);
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  if (fn == "constants") {
    std::string message = ErrorTextCryptoValue::constants();
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || this->cryptoArgs->cryptoOptions(args[i], shown, allowed, options);
  };

  if (fn == "createHash") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->cryptoArgs->cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    std::vector<Expr *> hashArgs = {md, length ? this->cryptoArgs->cryptoNumber(length)
                                               : this->cryptoArgs->cryptoDouble(loc, -1)};
    return this->cryptoArgs->cryptoResult(call, CryptoFn::HashNew, std::move(hashArgs),
                                          Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->cryptoArgs->cryptoMd(args[0], true);
    Expr *key = this->cryptoArgs->cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    std::vector<Expr *> hmacArgs = {md, key, this->cryptoArgs->cryptoInt(loc, 0)};
    return this->cryptoArgs->cryptoResult(call, CryptoFn::HmacNew, std::move(hmacArgs),
                                          Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->cryptoArgs->cryptoMd(args[0], false);
    Expr *data = this->cryptoArgs->cryptoData(args[1], "data", true, false, false);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 2, fn, 6);
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return this->cryptoArgs->cryptoResult(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::HashOnceText, {md, data, encoding},
                                          Type::string());
  }
  auto proven = [&](Expr *arg, int64_t lo, std::optional<int64_t> hi,
                    llvm::StringRef what) -> Expr * {
    Expr *e = this->exprChecker->checkExpr(arg, Type::intTy());
    if (e->type->isError()) {
      return nullptr;
    }
    IndexProofs::ResultBetweenProven between = this->indexProofs->requireBetween(e, lo, hi, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(e->loc(), between.error().message);
    }
    if (between.isErr() && !this->indexProofs->exempt(e->loc())) {
      return nullptr;
    }
    return e->type->isFloat() ? e : this->typeRelations->convert(e, Type::doubleTy());
  };
  if (fn == "randomBytes") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        this->cryptoArgs->rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    Expr *size = proven(args[0], 0, 2147483647, "the size");
    if (!size) {
      return call;
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomBytes, {size}, buffer);
  }
  if (fn == "randomUUID") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      this->argumentChecker->checkArg(cache, Type::boolTy());
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 &&
        this->cryptoArgs->rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      std::string message = ErrorTextCryptoFunction::randomArgs();
      this->state.getDiagnostics().error(args[2]->loc(), message);
      return loosely();
    }
    Expr *min = args.size() == 2 ? this->exprChecker->checkExpr(args[0], Type::intTy())
                                 : this->cryptoArgs->cryptoInt(loc, 0);
    Expr *max = this->exprChecker->checkExpr(args.back(), Type::intTy());
    if (min->type->isError() || max->type->isError()) {
      return call;
    }
    for (Expr *bound : {min, max}) {
      if (!bound->type->isInt()) {
        std::string message = ErrorTextCryptoFunction::randomInt(bound->type->name());
        this->state.getDiagnostics().error(bound->loc(), message);
        return call;
      }
    }
    std::string greater = ErrorTextCryptoFunction::greater(args.size() == 2);
    IndexProofs::ResultLessWasProven less = this->indexProofs->requireLess(min, max, greater);
    if (less.isErr()) {
      this->state.getDiagnostics().error(max->loc(), less.error().message);
      return call;
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    ArgumentChecker::ResultArityMatches arity =
        this->argumentChecker->checkArity(call, 1, fn == "randomFillSync" ? 3 : 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *target = this->exprChecker->checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    ResultFilledTarget filled = CryptoFunctionChecker::filledTarget(type, fn, shown);
    if (filled.isErr()) {
      this->state.getDiagnostics().error(target->loc(), filled.error().message);
      return call;
    }
    if (fn == "getRandomValues") {
      return this->cryptoArgs->cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = this->cryptoArgs->cryptoDouble(loc, 0);
    Expr *size = this->cryptoArgs->cryptoDouble(loc, 0);
    if (present(1)) {
      Expr *at = this->exprChecker->checkExpr(args[1], Type::intTy());
      bool library = this->indexProofs->exempt(at->loc());
      if (at->type->isError()) {
        return call;
      }
      if (!library) {
        std::string offsetWhat = ErrorTextCryptoFnLabel::offset();
        IndexProofs::ResultBetweenProven between =
            this->indexProofs->requireBetween(at, 0, {}, offsetWhat);
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(at->loc(), between.error().message);
        }
        if (between.isErr()) {
          return call;
        }
        IndexProofs::ResultUpToWasProven upTo =
            this->indexProofs->requireUpTo(at, target, 0, offsetWhat);
        if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(at->loc(), upTo.error().message);
        }
        if (upTo.isErr()) {
          return call;
        }
      }
      Expr *count = nullptr;
      if (present(2)) {
        count = this->exprChecker->checkExpr(args[2], Type::intTy());
        if (count->type->isError()) {
          return call;
        }
        if (!library) {
          std::string sizeWhat = ErrorTextCryptoFnLabel::size();
          IndexProofs::ResultBetweenProven sizeBetween =
              this->indexProofs->requireBetween(count, 0, {}, sizeWhat);
          if (sizeBetween.isErr() &&
              sizeBetween.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
            this->state.getDiagnostics().error(count->loc(), sizeBetween.error().message);
          }
          if (sizeBetween.isErr()) {
            return call;
          }
        }
        if (!this->indexProofs->provesSumUpTo(this->indexProofs->linear(at),
                                              this->indexProofs->linear(count), target) &&
            !this->indexProofs->exempt(count->loc())) {
          std::string message = ErrorTextCryptoFunction::fillFits(
              BytesCheckHelpers::valueText(count), BytesCheckHelpers::valueText(at));
          this->state.getDiagnostics().error(count->loc(), message);
          return call;
        }
        size =
            count->type->isFloat() ? count : this->typeRelations->convert(count, Type::doubleTy());
      }
      offset = at->type->isFloat() ? at : this->typeRelations->convert(at, Type::doubleTy());
    }
    return this->cryptoArgs->cryptoCall(
        call, CryptoFn::RandomFill, {target, offset, size, this->cryptoArgs->cryptoInt(loc, has)},
        type);
  }
  if (fn == "timingSafeEqual") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::string firstWhat = ErrorTextCryptoFnLabel::first();
    std::string secondWhat = ErrorTextCryptoFnLabel::second();
    Expr *a = this->cryptoArgs->cryptoData(args[0], firstWhat, false, false, true);
    Expr *b = this->cryptoArgs->cryptoData(args[1], secondWhat, false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b}, Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *password = this->cryptoArgs->cryptoData(args[0], "password", true, true, true);
    Expr *salt = this->cryptoArgs->cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = this->cryptoArgs->cryptoNumber(args[2]),
         *keylen = this->cryptoArgs->cryptoNumber(args[3]);
    Expr *md = this->cryptoArgs->cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError() ||
        !this->cryptoArgs->checkWhole(iterations, "iterations", 1) ||
        !this->cryptoArgs->checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::Pbkdf2,
                                          {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 5, 5);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *md = this->cryptoArgs->cryptoMd(args[0], true);
    Expr *key = this->cryptoArgs->cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = this->cryptoArgs->cryptoData(args[2], "salt", true, false, true);
    Expr *info = this->cryptoArgs->cryptoData(args[3], "info", true, false, true);
    Expr *keylen = this->cryptoArgs->cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError() ||
        !this->cryptoArgs->checkWhole(keylen, "keylen", 1)) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                                          Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *password = this->cryptoArgs->cryptoData(args[0], "password", true, true, true);
    Expr *salt = this->cryptoArgs->cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = this->cryptoArgs->cryptoNumber(args[2]);
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
        std::string message = ErrorTextCryptoFunction::together(pairs[i].first, pairs[i].second);
        this->state.getDiagnostics().error(b->loc(), message);
        return call;
      }
      values[i] = a || b ? this->cryptoArgs->cryptoNumber(a ? a : b)
                         : this->cryptoArgs->cryptoDouble(loc, 0);
    }
    if (std::optional<double> cost = this->cryptoArgs->knownNumber(values[0])) {
      int64_t n = int64_t(*cost);
      if (*cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0)) {
        std::string costShown = CryptoArgs::numberText(*cost);
        std::string message = ErrorTextCryptoFunction::costN(costShown);
        this->state.getDiagnostics().error(values[0]->loc(), message);
        return call;
      }
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(
        call, CryptoFn::Scrypt,
        {password, salt, keylen, values[0], values[1], values[2],
         maxmem ? this->cryptoArgs->cryptoNumber(maxmem) : this->cryptoArgs->cryptoDouble(loc, 0)},
        buffer);
  }
  if (fn == "argon2Sync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *type, *name;
    if (CryptoFunctionChecker::literalOf(args[0])) {
      std::string argon = ErrorTextCryptoFnLabel::argon();
      int which =
          this->cryptoArgs->cryptoChoice(args[0], argon, {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = this->cryptoArgs->cryptoInt(loc, which);
      name = this->cryptoArgs->cryptoString(loc, "");
    } else {
      type = this->cryptoArgs->cryptoInt(loc, 0xffffffff);
      name = this->argumentChecker->checkArg(args[0], Type::string());
    }
    if (!this->cryptoArgs->cryptoOptions(args[1], shown,
                                         {"message", "nonce", "parallelism", "tagLength", "memory",
                                          "passes", "secret", "associatedData"},
                                         options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        std::string message = ErrorTextCryptoFunction::needs(shown, required);
        this->state.getDiagnostics().error(args[1]->loc(), message);
        return call;
      }
    }
    Expr *message = this->cryptoArgs->cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = this->cryptoArgs->cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = this->cryptoArgs->cryptoNumber(options["parallelism"]);
    Expr *tagLength = this->cryptoArgs->cryptoNumber(options["tagLength"]);
    Expr *memory = this->cryptoArgs->cryptoNumber(options["memory"]);
    Expr *passes = this->cryptoArgs->cryptoNumber(options["passes"]);
    Expr *secret =
        options.lookup("secret")
            ? this->cryptoArgs->cryptoData(options["secret"], "secret", true, false, true)
            : this->cryptoArgs->cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? this->cryptoArgs->cryptoData(options["associatedData"], "associatedData",
                                                    true, false, true)
                     : this->cryptoArgs->cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return this->cryptoArgs->cryptoResult(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 3, 4);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> name = CryptoFunctionChecker::literalOf(args[0]);
    Expr *cipher = this->cryptoArgs->cryptoCipher(args[0]);
    Expr *key = this->cryptoArgs->cryptoData(args[1], "key", true, true, true);
    Expr *iv = this->cryptoArgs->cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError() || !this->cryptoArgs->checkCipherLengths(cipher, key, iv)) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      std::string message = ErrorTextCryptoFunction::tagLength(*name);
      this->state.getDiagnostics().error(args[0]->loc(), message);
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return this->cryptoArgs->cryptoResult(
        call, CryptoFn::CipherNew,
        {cipher, key, iv,
         tag ? this->cryptoArgs->cryptoNumber(tag) : this->cryptoArgs->cryptoDouble(loc, -1),
         this->cryptoArgs->cryptoBool(loc, decrypt)},
        Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return this->cryptoArgs->cryptoCall(call,
                                        fn == "getHashes"    ? CryptoFn::GetHashes
                                        : fn == "getCiphers" ? CryptoFn::GetCiphers
                                                             : CryptoFn::GetCurves,
                                        {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *key = this->cryptoArgs->cryptoData(args[0], "key", true, false, true);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->cryptoArgs->cryptoInt(loc, 0);
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = this->cryptoArgs->cryptoInt(loc, 0),
               *type = this->cryptoArgs->cryptoString(loc, "");
    Expr *passphrase = this->cryptoArgs->cryptoUndefined(loc);
    int format = 0;
    int has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!this->cryptoArgs->cryptoOptions(
              args[0], fn, {"key", "format", "type", "passphrase", "encoding"}, options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        std::string keyFormat = ErrorTextCryptoFnLabel::keyFormat();
        format = this->cryptoArgs->cryptoChoice(f, keyFormat, {"pem", "der", "jwk"});
        if (format < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        std::string message = ErrorTextCryptoOptions::needsKey(fn);
        this->state.getDiagnostics().error(args[0]->loc(), message);
        return call;
      }
      key = format == 2 ? this->argumentChecker->checkArg(options["key"], Type::json())
                        : this->cryptoArgs->cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        std::string keyType = ErrorTextCryptoFnLabel::keyType();
        int which = this->cryptoArgs->cryptoChoice(t, keyType, {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = this->cryptoArgs->cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = this->cryptoArgs->cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = CryptoFunctionChecker::literalOf(e)
                           ? this->bytesChecker->checkBufferEncoding(e)
                           : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          std::string message = ErrorTextCryptoFunction::keyCoding();
          this->state.getDiagnostics().error(e->loc(), message);
          return call;
        }
        encoding = number;
      }
    } else {
      key = this->cryptoArgs->cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::CreateKey,
                                          {this->cryptoArgs->cryptoBool(loc, priv), key, encoding,
                                           this->cryptoArgs->cryptoInt(loc, format),
                                           this->cryptoArgs->cryptoInt(loc, has), type, passphrase},
                                          keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return this->keyPairChecker->checkGenerateKeyPair(call, loc, shown);
  }
  if (fn == "generateKeySync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::string keyType = ErrorTextCryptoFnLabel::keyType();
    int which = this->cryptoArgs->cryptoChoice(args[0], keyType, {"hmac", "aes"});
    if (which < 0 || !this->cryptoArgs->cryptoOptions(args[1], shown, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      std::string message = ErrorTextCryptoFunction::keyLength(shown);
      this->state.getDiagnostics().error(args[1]->loc(), message);
      return call;
    }
    Expr *bits = this->exprChecker->checkExpr(options["length"], Type::intTy());
    if (bits->type->isError()) {
      return call;
    }
    if (which == 1) {
      std::optional<int64_t> known = this->constantFolder->intConstant(bits);
      if (!known || (*known != 128 && *known != 192 && *known != 256)) {
        std::string message = ErrorTextCryptoFunction::aesLength();
        this->state.getDiagnostics().error(bits->loc(), message);
        return call;
      }
    } else {
      std::string keyBits = ErrorTextCryptoFnLabel::keyBits();
      IndexProofs::ResultBetweenProven between =
          this->indexProofs->requireBetween(bits, 8, {}, keyBits);
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(bits->loc(), between.error().message);
      }
      if (between.isErr() && !this->indexProofs->exempt(bits->loc())) {
        return call;
      }
    }
    return this->cryptoArgs->cryptoCall(
        call, CryptoFn::GenerateSecretKey,
        {this->cryptoArgs->cryptoBool(loc, which == 1),
         bits->type->isFloat() ? bits : this->typeRelations->convert(bits, Type::doubleTy())},
        keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    ArgumentChecker::ResultArityMatches arity =
        this->argumentChecker->checkArity(call, unsigned(count), unsigned(count + 1));
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || this->cryptoArgs->rejectCryptoCallback(
                             call, count, fn,
                             verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm = isa<UndefinedExpr>(args[0])
                          ? this->cryptoArgs->cryptoUndefined(loc)
                          : this->argumentChecker->checkArg(args[0], Type::string());
    Expr *data = this->cryptoArgs->cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!this->cryptoArgs->cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature = this->cryptoArgs->cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, verify ? CryptoFn::Verify : CryptoFn::Sign,
                                          std::move(signArgs), verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *name = args[0];
    bool literal = CryptoFunctionChecker::literalOf(name).has_value();
    if (literal && !this->cryptoArgs->cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    std::vector<Expr *> signArgs = {this->argumentChecker->checkArg(name, Type::string()),
                                    this->cryptoArgs->cryptoBool(loc, verify)};
    const Type *made = Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign);
    return this->cryptoArgs->cryptoResult(call, CryptoFn::SignNew, std::move(signArgs), made);
  }
  if (fn == "createECDH") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::optional<std::string> curve = CryptoFunctionChecker::literalOf(args[0]);
    ResultCurveIsKnown known =
        curve ? CryptoFunctionChecker::curveIsKnown(*curve) : ok(llvm::StringRef());
    if (known.isErr()) {
      this->state.getDiagnostics().error(args[0]->loc(), known.error().message);
      return loosely();
    }
    Expr *name = this->argumentChecker->checkArg(args[0], Type::string());
    return this->cryptoArgs->cryptoResult(call, CryptoFn::EcdhNew, {name},
                                          Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() ||
        !this->cryptoArgs->cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      std::string message = ErrorTextCryptoFunction::dhKeys();
      this->state.getDiagnostics().error(args[0]->loc(), message);
      return call;
    }
    std::string privateWhat = ErrorTextCryptoFnLabel::privateKey();
    std::string publicWhat = ErrorTextCryptoFnLabel::publicKey();
    Expr *priv = this->cryptoArgs->cryptoKeyObject(options["privateKey"], privateWhat);
    Expr *pub = this->cryptoArgs->cryptoKeyObject(options["publicKey"], publicWhat);
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  if (fn == "generatePrimeSync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"add", "rem", "safe", "bigint"})) {
      return call;
    }
    if (Expr *big = options.lookup("bigint")) {
      std::string message = ErrorTextCryptoFunction::bigint();
      this->state.getDiagnostics().error(big->loc(), message);
      return call;
    }
    Expr *size = this->cryptoArgs->cryptoNumber(args[0]);
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
  if (fn == "checkPrimeSync") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (arity.isErr() || !optionsAt(1, {"checks"})) {
      return call;
    }
    Expr *candidate = this->cryptoArgs->cryptoData(args[0], "candidate", false, false, true);
    Expr *checks = options.lookup("checks") ? this->cryptoArgs->cryptoNumber(options["checks"])
                                            : this->cryptoArgs->cryptoDouble(loc, 0);
    if (candidate->type->isError() || checks->type->isError() ||
        !this->cryptoArgs->checkWhole(checks, "checks", 0)) {
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::CheckPrime, {candidate, checks},
                                          Type::boolTy());
  }
  CryptoExports::ResultExportFound found = CryptoExports::cryptoExport(shown, "node:crypto");
  std::string message = found.isErr() ? found.error().message : std::string();
  this->state.getDiagnostics().error(loc, message);

  return loosely();
}

}
