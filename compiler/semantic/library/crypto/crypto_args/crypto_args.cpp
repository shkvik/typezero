#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/semantic/library/crypto/crypto_errors/crypto_errors.h"
#include "compiler/semantic/library/crypto/crypto_names/crypto_names.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FormatVariadic.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CryptoArgs::CryptoArgs(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->bytesChecker = &all.bytesChecker;
  this->cryptoErrors = &all.cryptoErrors;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::u16string CryptoArgs::utf16Of(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::optional<std::string> CryptoArgs::literalOf(const Expr *e) {
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

Expr *CryptoArgs::cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                             const Type *type) {
  auto *index = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(fn));
  index->type = Type::intTy();
  args.insert(args.begin(), index);
  call->args = std::move(args);
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::Crypto};
  call->type = type;

  return call;
}

Expr *CryptoArgs::cryptoResult(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                               const Type *type) {
  const Type *error = this->cryptoErrors->ofCall(fn, args);

  return this->cryptoCall(call, fn, std::move(args), Type::result(type, error));
}

Expr *CryptoArgs::cryptoInt(llvm::SMLoc loc, int64_t value) {
  auto *lit = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
  lit->type = Type::intTy();

  return lit;
}

Expr *CryptoArgs::cryptoDouble(llvm::SMLoc loc, double value) {
  auto *lit = this->state.getAst().make<FloatLitExpr>(loc, value);
  lit->type = Type::doubleTy();

  return lit;
}

Expr *CryptoArgs::cryptoBool(llvm::SMLoc loc, bool value) {
  auto *lit = this->state.getAst().make<BoolLitExpr>(loc, value);
  lit->type = Type::boolTy();

  return lit;
}

Expr *CryptoArgs::cryptoString(llvm::SMLoc loc, llvm::StringRef text) {
  auto *lit = this->state.getAst().make<StringLitExpr>(loc, CryptoArgs::utf16Of(text));
  lit->type = Type::string();

  return lit;
}

Expr *CryptoArgs::cryptoUndefined(llvm::SMLoc loc) {
  auto *undef = this->state.getAst().make<UndefinedExpr>(loc);
  undef->type = Type::undefinedTy();

  return undef;
}

Expr *CryptoArgs::cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys,
                             bool buffers) {
  arg = this->exprChecker->checkExpr(arg, Type::string());
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
    std::string message = ErrorTextCryptoData::notData(what, strings, buffers, keys, type);
    this->state.getDiagnostics().error(arg->loc(), message);
    arg->type = Type::error();
  }
  if (type->isStringLiteral()) {
    return this->typeRelations->coerce(arg, Type::string());
  }

  return arg;
}

Expr *CryptoArgs::cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef fn, int fallback) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return this->cryptoInt(call->loc(), fallback);
  }
  Expr *arg = call->args[index];
  std::optional<std::string> text = CryptoArgs::literalOf(arg);
  if (!text) {
    this->exprChecker->checkExpr(arg);
    std::string message = ErrorTextCryptoData::encoding(fn);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  if (*text == "buffer") {
    return this->cryptoInt(call->loc(), -1);
  }
  Expr *number = this->bytesChecker->checkBufferEncoding(arg);
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
  auto *member = this->state.getAst().make<MemberExpr>(made->loc(), made, "toString");

  return this->processChecker->builtinCall(made->loc(), member, Builtin::BufferToString, {encoding},
                                           Type::string());
}

bool CryptoArgs::cryptoOptions(Expr *arg, llvm::StringRef fn,
                               llvm::ArrayRef<llvm::StringRef> allowed,
                               llvm::StringMap<Expr *> &options) {
  auto *lit = dyn_cast<ObjectLitExpr>(arg);
  if (!lit) {
    this->exprChecker->checkExpr(arg);
    std::string message = ErrorTextCryptoOptions::literal(fn, allowed[0]);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread() || prop.key() || prop.method() || prop.name.empty()) {
      std::string message = ErrorTextCryptoOptions::byName(fn, allowed[0]);
      this->state.getDiagnostics().error(prop.loc, message);
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      std::string message = ErrorTextCryptoOptions::unknown(fn, prop.name, allowed);
      this->state.getDiagnostics().error(prop.loc, message);
      return false;
    }
    options[prop.name] = prop.value;
  }

  return true;
}

int CryptoArgs::cryptoChoice(Expr *arg, llvm::StringRef what,
                             llvm::ArrayRef<llvm::StringRef> values) {
  std::optional<std::string> text = CryptoArgs::literalOf(arg);
  if (!text) {
    this->exprChecker->checkExpr(arg);
    std::string message = ErrorTextCryptoChoice::literal(what, values);
    this->state.getDiagnostics().error(arg->loc(), message);
    return -1;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] == *text) {
      return int(i);
    }
  }
  std::string message = ErrorTextCryptoChoice::unknown(what, *text, values);
  this->state.getDiagnostics().error(arg->loc(), message);

  return -1;
}

Expr *CryptoArgs::cryptoMd(Expr *arg, bool hmac) {
  std::optional<std::string> text = CryptoArgs::literalOf(arg);
  if (!text) {
    return this->argumentChecker->checkArg(arg, Type::string());
  }
  std::string name = llvm::StringRef(*text).lower();
  for (const HashNames &h : CryptoNames::hashNames()) {
    if (h.name != name && !llvm::is_contained(h.aliases, name)) {
      continue;
    }
    if (hmac && (h.name == "shake128" || h.name == "shake256")) {
      std::string message = ErrorTextCryptoHash::anyLength(*text);
      this->state.getDiagnostics().error(arg->loc(), message);
      return nullptr;
    }
    for (size_t i = 0; i < std::size(kDirectMds); ++i) {
      if (kDirectMds[i].name == h.name) {
        return this->cryptoInt(arg->loc(), int64_t(i));
      }
    }
  }
  if (CryptoNames::turnedOffHash(name)) {
    std::string message = ErrorTextCryptoHash::turnedOff(*text);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  std::string message = ErrorTextCryptoHash::unknown(*text);
  this->state.getDiagnostics().error(arg->loc(), message);

  return nullptr;
}

Expr *CryptoArgs::cryptoCipher(Expr *arg) {
  std::optional<std::string> text = CryptoArgs::literalOf(arg);
  if (!text) {
    return this->argumentChecker->checkArg(arg, Type::string());
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
      return this->cryptoInt(arg->loc(), int64_t(i));
    }
  }
  if (llvm::is_contained(kCipherNames, name)) {
    return this->cryptoString(arg->loc(), name);
  }
  if (CryptoNames::turnedOffCipher(name)) {
    std::string message = ErrorTextCryptoCipher::turnedOff(*text);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  std::string message = ErrorTextCryptoCipher::unknown(*text);
  this->state.getDiagnostics().error(arg->loc(), message);

  return nullptr;
}

Expr *CryptoArgs::cryptoNumber(Expr *arg) {
  Expr *checked = this->argumentChecker->checkArg(arg, Type::intTy());
  if (!checked || checked->type->isError() || checked->type->isFloat()) {
    return checked;
  }

  return this->typeRelations->convert(checked, Type::doubleTy());
}

std::string CryptoArgs::numberText(double value) {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(int64_t(value));
  }

  return llvm::formatv("{0:g}", value).str();
}

std::optional<double> CryptoArgs::knownNumber(const Expr *arg) const {
  std::vector<const Expr *> negations;
  const Expr *inner = arg;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    negations.push_back(inner);
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  std::optional<double> value = this->plainNumber(inner);
  for (const Expr *negated : llvm::reverse(negations)) {
    if (value.has_value()) {
      value = -*value;
    } else {
      value = this->plainNumber(negated);
    }
  }

  return value;
}

std::optional<double> CryptoArgs::plainNumber(const Expr *arg) const {
  if (auto *lit = dyn_cast<FloatLitExpr>(arg)) {
    return lit->value;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(arg)) {
    return double(lit->value);
  }
  if (auto *conversion = dyn_cast<CastExpr>(arg)) {
    if (std::optional<int64_t> value = this->constantFolder->intConstant(conversion->operand)) {
      return double(*value);
    }
  }
  std::optional<int64_t> value = this->constantFolder->intConstant(arg);
  if (!value) {
    return std::nullopt;
  }

  return double(*value);
}

bool CryptoArgs::checkWhole(const Expr *arg, llvm::StringRef name, double least) {
  std::optional<double> value = this->knownNumber(arg);
  if (!value || (*value == std::floor(*value) && *value >= least)) {
    return true;
  }
  std::string leastShown = CryptoArgs::numberText(least);
  std::string valueShown = CryptoArgs::numberText(*value);
  std::string message = ErrorTextCryptoNumber::whole(name, leastShown, valueShown);
  this->state.getDiagnostics().error(arg->loc(), message);

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

    return this->indexProofs->exactLength(e);
  };
  bool ok = true;
  if (std::optional<int64_t> have = bytesOf(key); have && *have != keyBytes) {
    std::string message = ErrorTextCryptoCipher::keyBytes(name, keyBytes, *have);
    this->state.getDiagnostics().error(key->loc(), message);
    ok = false;
  }
  std::optional<int64_t> ivHave = bytesOf(iv);
  if (ivHave && anyIv && *ivHave == 0) {
    std::string message = ErrorTextCryptoCipher::emptyIv(name);
    this->state.getDiagnostics().error(iv->loc(), message);
    ok = false;
  } else if (ivHave && !anyIv && *ivHave != ivBytes) {
    std::string message = ErrorTextCryptoCipher::ivBytes(name, ivBytes, *ivHave);
    this->state.getDiagnostics().error(iv->loc(), message);
    ok = false;
  }

  return ok;
}

Expr *CryptoArgs::cryptoKey(Expr *arg, llvm::StringRef what) {
  return this->cryptoData(arg, what, true, true, true);
}

CryptoArgs::ResultKeyObjectType CryptoArgs::keyObjectType(const Type *type, llvm::StringRef what) {
  if (!type->isError() && type->crypto() != Crypto::KeyObject) {
    std::string message = ErrorTextCryptoKey::keyObject(what, type->name());

    return err(CryptoArgsKeyObjectTypeError{
        .code = CryptoArgsKeyObjectTypeErrorCode::NOT_A_KEY_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

Expr *CryptoArgs::cryptoKeyObject(Expr *arg, llvm::StringRef what) {
  arg = this->exprChecker->checkExpr(arg);
  ResultKeyObjectType key = CryptoArgs::keyObjectType(arg->type, what);
  if (key.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), key.error().message);
    arg->type = Type::error();
  }

  return arg;
}

bool CryptoArgs::rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn,
                                      llvm::StringRef hint) {
  if (index >= call->args.size()) {
    return false;
  }
  Expr *arg = this->exprChecker->checkExpr(call->args[index]);
  if (!arg->type->nonOptional()->isFunction()) {
    return false;
  }
  std::string message = ErrorTextCryptoKey::callback(fn, hint);
  this->state.getDiagnostics().error(arg->loc(), message);
  for (Expr *&a : call->args) {
    if (a != call->args[index]) {
      a = this->exprChecker->checkExpr(a);
    }
  }
  call->type = Type::error();

  return true;
}

bool CryptoArgs::cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv,
                               std::vector<Expr *> &args) {
  llvm::SMLoc loc = arg->loc();
  Expr *padding = this->cryptoInt(loc, 0);
  Expr *salt = this->cryptoInt(loc, 0);
  int p1363 = 0;
  int has = 0;
  Expr *key;
  if (isa<ObjectLitExpr>(arg)) {
    llvm::StringMap<Expr *> options;
    if (!this->cryptoOptions(arg, fn, {"key", "padding", "saltLength", "dsaEncoding"}, options)) {
      return false;
    }
    if (!options.lookup("key")) {
      std::string message = ErrorTextCryptoOptions::needsKey(fn);
      this->state.getDiagnostics().error(arg->loc(), message);
      return false;
    }
    std::string keyWhat = ErrorTextCryptoLabel::key();
    key = this->cryptoKey(options["key"], keyWhat);
    if (Expr *p = options.lookup("padding")) {
      padding = this->argumentChecker->checkArg(p, Type::intTy());
      has |= 1;
    }
    if (Expr *s = options.lookup("saltLength")) {
      salt = this->argumentChecker->checkArg(s, Type::intTy());
      has |= 2;
    }
    if (Expr *d = options.lookup("dsaEncoding")) {
      std::string signature = ErrorTextCryptoLabel::signature();
      int which = this->cryptoChoice(d, signature, {"der", "ieee-p1363"});
      if (which < 0) {
        return false;
      }
      p1363 = which;
    }
  } else {
    key = this->cryptoKey(arg, priv ? "private key" : "key");
  }
  if (key->type->isError()) {
    return false;
  }
  args.insert(args.end(),
              {key, padding, salt, this->cryptoInt(loc, p1363), this->cryptoInt(loc, has)});

  return true;
}

}
