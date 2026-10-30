#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FormatVariadic.h"
#include <cmath>

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

std::u16string utf16Of(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
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

CryptoArgs::CryptoArgs(Checker &checker) : checker_(checker) {}

Expr *CryptoArgs::cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                             const Type *type) {
  auto *index = checker_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(fn));
  index->type = Type::intTy();
  args.insert(args.begin(), index);
  call->args = std::move(args);
  call->spread.clear();
  call->builtin = Builtin::Crypto;
  call->type = type;

  return call;
}

Expr *CryptoArgs::cryptoResult(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                               const Type *type) {
  const Type *error = checker_.cryptoErrorTypes().ofCall(fn, args);

  return cryptoCall(call, fn, std::move(args), Type::result(type, error));
}

Expr *CryptoArgs::cryptoInt(llvm::SMLoc loc, int64_t value) {
  auto *lit = checker_.ctx_.make<IntLitExpr>(loc, uint64_t(value));
  lit->type = Type::intTy();

  return lit;
}

Expr *CryptoArgs::cryptoDouble(llvm::SMLoc loc, double value) {
  auto *lit = checker_.ctx_.make<FloatLitExpr>(loc, value);
  lit->type = Type::doubleTy();

  return lit;
}

Expr *CryptoArgs::cryptoBool(llvm::SMLoc loc, bool value) {
  auto *lit = checker_.ctx_.make<BoolLitExpr>(loc, value);
  lit->type = Type::boolTy();

  return lit;
}

Expr *CryptoArgs::cryptoString(llvm::SMLoc loc, llvm::StringRef text) {
  auto *lit = checker_.ctx_.make<StringLitExpr>(loc, utf16Of(text));
  lit->type = Type::string();

  return lit;
}

Expr *CryptoArgs::cryptoUndefined(llvm::SMLoc loc) {
  auto *undef = checker_.ctx_.make<UndefinedExpr>(loc);
  undef->type = Type::undefinedTy();

  return undef;
}

Expr *CryptoArgs::cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys,
                             bool buffers) {
  arg = checker_.exprChecker().checkExpr(arg, Type::string());
  auto fits = [&](const Type *t) {
    return (strings && (t->isString() || t->isStringLiteral())) || t->isByteView() ||
           (buffers && t->bytes() == Type::Bytes::ArrayBuffer) ||
           (keys && t->crypto() == Crypto::KeyObject);
  };
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  bool ok = fits(type) || (type->isUnion() && llvm::all_of(type->members(), fits));
  if (!ok) {
    std::string kinds = strings ? "a string or bytes" : "bytes";
    checker_.diags_.error(arg->loc(),
                          "the " + what.str() + " is " + kinds +
                              " (a Buffer, a typed array or a DataView" +
                              (buffers ? ", an ArrayBuffer" : "") + (keys ? ", a KeyObject" : "") +
                              "); found '" + type->name().str() + "'" +
                              (type->isNumeric() ? "; make text of it with 'String(x)'" : ""));
    arg->type = Type::error();
  }
  if (type->isStringLiteral()) {
    return checker_.typeRelations().coerce(arg, Type::string());
  }

  return arg;
}

Expr *CryptoArgs::cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef fn, int fallback) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return cryptoInt(call->loc(), fallback);
  }
  Expr *arg = call->args[index];
  std::optional<std::string> text = literalOf(arg);
  if (!text) {
    checker_.exprChecker().checkExpr(arg);
    checker_.diags_.error(arg->loc(), "the encoding of '" + fn +
                                          "' is written as a literal, e.g. 'hex': "
                                          "the type of what it gives depends on it");
    return nullptr;
  }
  if (*text == "buffer") {
    return cryptoInt(call->loc(), -1);
  }
  Expr *number = checker_.bytesChecker().checkBufferEncoding(arg);
  if (number->type->isError()) {
    return nullptr;
  }

  return number;
}

Expr *CryptoArgs::cryptoText(Expr *made, Expr *encoding) {
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    return made;
  }
  if (made->type->isResult()) {
    auto *crypto = cast<CallExpr>(made);
    crypto->args.push_back(encoding);
    crypto->type = Type::result(Type::string(), made->type->errType());

    return crypto;
  }
  auto *member = checker_.ctx_.make<MemberExpr>(made->loc(), made, "toString");

  return checker_.processChecker().builtinCall(made->loc(), member, Builtin::BufferToString,
                                               {encoding}, Type::string());
}

bool CryptoArgs::cryptoOptions(Expr *arg, llvm::StringRef fn,
                               llvm::ArrayRef<llvm::StringRef> allowed,
                               llvm::StringMap<Expr *> &options) {
  auto *lit = dyn_cast<ObjectLitExpr>(arg);
  if (!lit) {
    checker_.exprChecker().checkExpr(arg);
    checker_.diags_.error(arg->loc(),
                          "the options of '" + fn +
                              "' are written in the call, as an object literal, e.g. '{ " +
                              allowed[0].str() + ": ... }'");
    return false;
  }
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.key || prop.method || prop.name.empty()) {
      checker_.diags_.error(prop.loc, "write each option of '" + fn + "' by its name, e.g. '{ " +
                                          allowed[0].str() + ": ... }'");
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      checker_.diags_.error(prop.loc,
                            "'" + fn + "' has no option '" + prop.name + "'; it takes " + list);
      return false;
    }
    options[prop.name] = prop.value;
  }

  return true;
}

int CryptoArgs::cryptoChoice(Expr *arg, llvm::StringRef what,
                             llvm::ArrayRef<llvm::StringRef> values) {
  std::optional<std::string> text = literalOf(arg);
  std::string list;
  for (size_t i = 0; i < values.size(); ++i) {
    list += (i == 0 ? "'" : i + 1 == values.size() ? "' or '" : "', '") + values[i].str();
  }
  list += "'";
  if (!text) {
    checker_.exprChecker().checkExpr(arg);
    checker_.diags_.error(arg->loc(), "the " + what + " is written as a literal: " + list);
    return -1;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] == *text) {
      return int(i);
    }
  }
  checker_.diags_.error(arg->loc(), "unknown " + what + " '" + *text + "'; it is " + list);

  return -1;
}

Expr *CryptoArgs::cryptoMd(Expr *arg, bool hmac) {
  std::optional<std::string> text = literalOf(arg);
  if (!text) {
    return checker_.argumentChecker().checkArg(arg, Type::string());
  }
  std::string name = llvm::StringRef(*text).lower();
  for (const HashNames &h : hashNames()) {
    if (h.name != name && !llvm::is_contained(h.aliases, name)) {
      continue;
    }
    if (hmac && (h.name == "shake128" || h.name == "shake256")) {
      checker_.diags_.error(arg->loc(), "'" + *text +
                                            "' gives output of any length, which HMAC and the "
                                            "KDFs cannot use; use a hash such as 'sha256'");
      return nullptr;
    }
    for (size_t i = 0; i < std::size(kDirectMds); ++i) {
      if (kDirectMds[i].name == h.name) {
        return cryptoInt(arg->loc(), int64_t(i));
      }
    }
  }
  if (turnedOffHash(name)) {
    checker_.diags_.error(arg->loc(), "'" + *text +
                                          "' is turned off in node 24 (it throws "
                                          "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'sha256'");
    return nullptr;
  }
  checker_.diags_.error(arg->loc(), "unknown hash '" + *text +
                                        "'; node has those getHashes() lists, e.g. "
                                        "'sha256', 'sha512', 'sha3-256', 'md5'");

  return nullptr;
}

Expr *CryptoArgs::cryptoCipher(Expr *arg) {
  std::optional<std::string> text = literalOf(arg);
  if (!text) {
    return checker_.argumentChecker().checkArg(arg, Type::string());
  }
  std::string name = llvm::StringRef(*text).lower();
  std::string canonical = name == "aes128"          ? "aes-128-cbc"
                          : name == "aes192"        ? "aes-192-cbc"
                          : name == "aes256"        ? "aes-256-cbc"
                          : name == "id-aes128-gcm" ? "aes-128-gcm"
                          : name == "id-aes192-gcm" ? "aes-192-gcm"
                          : name == "id-aes256-gcm" ? "aes-256-gcm"
                                                    : name;
  for (size_t i = 0; i < std::size(kDirectCiphers); ++i) {
    if (kDirectCiphers[i].name == canonical) {
      return cryptoInt(arg->loc(), int64_t(i));
    }
  }
  if (llvm::is_contained(kCipherNames, name)) {
    return cryptoString(arg->loc(), name);
  }
  if (turnedOffCipher(name)) {
    checker_.diags_.error(arg->loc(), "'" + *text +
                                          "' is turned off in node 24 (it throws "
                                          "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use "
                                          "'aes-256-gcm'");
    return nullptr;
  }
  checker_.diags_.error(arg->loc(), "unknown cipher '" + *text +
                                        "'; node has those getCiphers() lists, "
                                        "e.g. 'aes-256-gcm', 'aes-256-cbc', "
                                        "'chacha20-poly1305'");

  return nullptr;
}

Expr *CryptoArgs::cryptoNumber(Expr *arg) {
  return checker_.argumentChecker().checkArg(arg, Type::doubleTy());
}

std::string CryptoArgs::numberText(double value) {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(int64_t(value));
  }

  return llvm::formatv("{0:g}", value).str();
}

std::optional<double> CryptoArgs::knownNumber(const Expr *arg) const {
  if (auto *lit = dyn_cast<FloatLitExpr>(arg)) {
    return lit->value;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(arg)) {
    return double(lit->value);
  }
  if (auto *unary = dyn_cast<UnaryExpr>(arg); unary && unary->op == UnaryOp::Neg) {
    std::optional<double> inner = knownNumber(unary->operand);
    if (inner) {
      return -*inner;
    }
  }
  if (auto *conversion = dyn_cast<CastExpr>(arg)) {
    if (std::optional<int64_t> value = checker_.constantFolder().intConstant(conversion->operand)) {
      return double(*value);
    }
  }
  std::optional<int64_t> value = checker_.constantFolder().intConstant(arg);
  if (!value) {
    return std::nullopt;
  }

  return double(*value);
}

bool CryptoArgs::checkWhole(const Expr *arg, llvm::StringRef name, double least) {
  std::optional<double> value = knownNumber(arg);
  if (!value || (*value == std::floor(*value) && *value >= least)) {
    return true;
  }
  checker_.diags_.error(arg->loc(), name + " must be a whole number of " + numberText(least) +
                                        " or more, not " + numberText(*value));

  return false;
}

bool CryptoArgs::checkCipherLengths(const Expr *cipher, const Expr *key, const Expr *iv) {
  auto *index = dyn_cast<IntLitExpr>(cipher);
  if (!index) {
    return true;
  }
  llvm::StringRef name = kDirectCiphers[index->value].name;
  int64_t keyBytes = name.contains("128") ? 16 : name.contains("192") ? 24 : 32;
  bool anyIv = name.ends_with("gcm");
  int64_t ivBytes = name.starts_with("chacha") ? 12 : 16;
  auto bytesOf = [&](const Expr *e) -> std::optional<int64_t> {
    Type::Bytes kind = e->type->bytes();
    if (kind != Type::Bytes::Buffer && kind != Type::Bytes::Uint8) {
      return std::nullopt;
    }

    return checker_.indexProofs().exactLength(e);
  };
  bool ok = true;
  if (std::optional<int64_t> have = bytesOf(key); have && *have != keyBytes) {
    checker_.diags_.error(key->loc(), name + " needs a " + std::to_string(keyBytes) +
                                          "-byte key, but this one has " + std::to_string(*have) +
                                          " bytes");
    ok = false;
  }
  std::optional<int64_t> ivHave = bytesOf(iv);
  if (ivHave && anyIv && *ivHave == 0) {
    checker_.diags_.error(iv->loc(), name + " needs an IV of 1 byte or more, but it is empty");
    ok = false;
  } else if (ivHave && !anyIv && *ivHave != ivBytes) {
    checker_.diags_.error(iv->loc(), name + " needs a " + std::to_string(ivBytes) +
                                         "-byte IV, but this one has " + std::to_string(*ivHave) +
                                         " bytes");
    ok = false;
  }

  return ok;
}

Expr *CryptoArgs::cryptoKey(Expr *arg, llvm::StringRef what) {
  return cryptoData(arg, what, true, true, true);
}

Expr *CryptoArgs::cryptoKeyObject(Expr *arg, llvm::StringRef what) {
  arg = checker_.exprChecker().checkExpr(arg);
  if (!arg->type->isError() && arg->type->crypto() != Crypto::KeyObject) {
    checker_.diags_.error(arg->loc(),
                          "the " + what + " is a KeyObject; found '" + arg->type->name() + "'");
    arg->type = Type::error();
  }

  return arg;
}

bool CryptoArgs::rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn,
                                      llvm::StringRef hint) {
  if (index >= call->args.size()) {
    return false;
  }
  Expr *arg = checker_.exprChecker().checkExpr(call->args[index]);
  if (!arg->type->nonOptional()->isFunction()) {
    return false;
  }
  checker_.diags_.error(arg->loc(),
                        "'" + fn +
                            "' with a callback is not supported: tz has no callback forms "
                            "of node:crypto (docs/crypto.md, section 5); call it without one "
                            "and use what it returns: " +
                            hint);
  for (Expr *&a : call->args) {
    if (a != call->args[index]) {
      a = checker_.exprChecker().checkExpr(a);
    }
  }
  call->type = Type::error();

  return true;
}

bool CryptoArgs::cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv,
                               std::vector<Expr *> &args) {
  llvm::SMLoc loc = arg->loc();
  Expr *padding = cryptoInt(loc, 0), *salt = cryptoInt(loc, 0);
  int p1363 = 0, has = 0;
  Expr *key;
  if (isa<ObjectLitExpr>(arg)) {
    llvm::StringMap<Expr *> options;
    if (!cryptoOptions(arg, fn, {"key", "padding", "saltLength", "dsaEncoding"}, options)) {
      return false;
    }
    if (!options.lookup("key")) {
      checker_.diags_.error(arg->loc(), "the options of '" + fn + "' need 'key'");
      return false;
    }
    key = cryptoKey(options["key"], "key");
    if (Expr *p = options.lookup("padding")) {
      padding = checker_.argumentChecker().checkArg(p, Type::intTy());
      has |= 1;
    }
    if (Expr *s = options.lookup("saltLength")) {
      salt = checker_.argumentChecker().checkArg(s, Type::intTy());
      has |= 2;
    }
    if (Expr *d = options.lookup("dsaEncoding")) {
      int which = cryptoChoice(d, "encoding of the signature", {"der", "ieee-p1363"});
      if (which < 0) {
        return false;
      }
      p1363 = which;
    }
  } else {
    key = cryptoKey(arg, priv ? "private key" : "key");
  }
  if (key->type->isError()) {
    return false;
  }
  args.insert(args.end(), {key, padding, salt, cryptoInt(loc, p1363), cryptoInt(loc, has)});

  return true;
}

}
