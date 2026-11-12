#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf16/to_utf16.h"
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

std::optional<std::string> CryptoArgs::literalOf(const Expr *expr) const {
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
  const auto *error = this->cryptoErrors->ofCall(fn, args);

  return this->cryptoCall(call, fn, std::move(args), Type::result(type, error));
}

Expr *CryptoArgs::cryptoInt(llvm::SMLoc loc, int64_t value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

Expr *CryptoArgs::cryptoDouble(llvm::SMLoc loc, double value) {
  auto *literal = this->state.getAst().make<FloatLitExpr>(loc, value);
  literal->type = Type::doubleTy();

  return literal;
}

Expr *CryptoArgs::cryptoBool(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.getAst().make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

Expr *CryptoArgs::cryptoString(llvm::SMLoc loc, llvm::StringRef text) {
  auto *literal = this->state.getAst().make<StringLitExpr>(loc, ToUtf16::toUtf16(text));
  literal->type = Type::string();

  return literal;
}

Expr *CryptoArgs::cryptoUndefined(llvm::SMLoc loc) {
  auto *undef = this->state.getAst().make<UndefinedExpr>(loc);
  undef->type = Type::undefinedTy();

  return undef;
}

Expr *CryptoArgs::cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys,
                             bool buffers) {
  arg = this->exprChecker->checkExpr(arg, Type::string());
  auto fits = [&](const Type *candidate) {
    return (strings && (candidate->isString() || candidate->isStringLiteral())) ||
           candidate->isByteView() || (buffers && candidate->bytes() == Type::Bytes::ArrayBuffer) ||
           (keys && candidate->crypto() == Crypto::KeyObject);
  };
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  bool isOk = fits(type) || (type->isUnion() && llvm::all_of(type->members(), fits));
  if (!isOk) {
    auto message = ErrorTextCryptoData::notData(what, strings, buffers, keys, type);
    this->state.getDiagnostics().error(arg->loc(), message);
    arg->type = Type::error();
  }
  if (type->isStringLiteral()) {
    return this->typeRelations->coerce(arg, Type::string());
  }

  return arg;
}

Expr *CryptoArgs::cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef functionName,
                                 int fallback) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return this->cryptoInt(call->loc(), fallback);
  }
  Expr *arg = call->args[index];
  auto text = this->literalOf(arg);
  if (!text) {
    this->exprChecker->checkExpr(arg);
    auto message = ErrorTextCryptoData::encoding(functionName);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  if (*text == "buffer") {
    return this->cryptoInt(call->loc(), -1);
  }
  auto *number = this->bytesChecker->checkBufferEncoding(arg);
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

bool CryptoArgs::cryptoOptions(Expr *arg, llvm::StringRef functionName,
                               llvm::ArrayRef<llvm::StringRef> allowed,
                               llvm::StringMap<Expr *> &options) {
  auto *literal = dyn_cast<ObjectLitExpr>(arg);
  if (literal == nullptr) {
    this->exprChecker->checkExpr(arg);
    auto message = ErrorTextCryptoOptions::literal(functionName, allowed[0]);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread() || property.key() || property.method() || property.name.empty()) {
      auto message = ErrorTextCryptoOptions::byName(functionName, allowed[0]);
      this->state.getDiagnostics().error(property.loc, message);
      return false;
    }
    if (!llvm::is_contained(allowed, property.name)) {
      auto message = ErrorTextCryptoOptions::unknown(functionName, property.name, allowed);
      this->state.getDiagnostics().error(property.loc, message);
      return false;
    }
    options[property.name] = property.value;
  }

  return true;
}

int CryptoArgs::cryptoChoice(Expr *arg, llvm::StringRef what,
                             llvm::ArrayRef<llvm::StringRef> values) {
  auto text = this->literalOf(arg);
  if (!text) {
    this->exprChecker->checkExpr(arg);
    auto message = ErrorTextCryptoChoice::literal(what, values);
    this->state.getDiagnostics().error(arg->loc(), message);
    return -1;
  }
  for (size_t index = 0; index < values.size(); ++index) {
    if (values[index] == *text) {
      return int(index);
    }
  }
  auto message = ErrorTextCryptoChoice::unknown(what, *text, values);
  this->state.getDiagnostics().error(arg->loc(), message);

  return -1;
}

Expr *CryptoArgs::cryptoMd(Expr *arg, bool hmac) {
  auto text = this->literalOf(arg);
  if (!text) {
    return this->argumentChecker->checkArg(arg, Type::string());
  }
  std::string name = llvm::StringRef(*text).lower();
  for (const HashNames &h : CryptoNames::hashNames()) {
    if (h.name != name && !llvm::is_contained(h.aliases, name)) {
      continue;
    }
    if (hmac && (h.name == "shake128" || h.name == "shake256")) {
      auto message = ErrorTextCryptoHash::anyLength(*text);
      this->state.getDiagnostics().error(arg->loc(), message);
      return nullptr;
    }
    for (size_t index = 0; index < std::size(kDirectMds); ++index) {
      if (kDirectMds[index].name == h.name) {
        return this->cryptoInt(arg->loc(), int64_t(index));
      }
    }
  }
  if (CryptoNames::turnedOffHash(name)) {
    auto message = ErrorTextCryptoHash::turnedOff(*text);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  auto message = ErrorTextCryptoHash::unknown(*text);
  this->state.getDiagnostics().error(arg->loc(), message);

  return nullptr;
}

Expr *CryptoArgs::cryptoCipher(Expr *arg) {
  auto text = this->literalOf(arg);
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
  for (size_t index = 0; index < std::size(kDirectCiphers); ++index) {
    if (kDirectCiphers[index].name == canonical) {
      return this->cryptoInt(arg->loc(), int64_t(index));
    }
  }
  if (llvm::is_contained(kCipherNames, name)) {
    return this->cryptoString(arg->loc(), name);
  }
  if (CryptoNames::turnedOffCipher(name)) {
    auto message = ErrorTextCryptoCipher::turnedOff(*text);
    this->state.getDiagnostics().error(arg->loc(), message);
    return nullptr;
  }
  auto message = ErrorTextCryptoCipher::unknown(*text);
  this->state.getDiagnostics().error(arg->loc(), message);

  return nullptr;
}

Expr *CryptoArgs::cryptoNumber(Expr *arg) {
  auto *checked = this->argumentChecker->checkArg(arg, Type::intTy());
  if (checked == nullptr || checked->type->isError() || checked->type->isFloat()) {
    return checked;
  }

  return this->typeRelations->convert(checked, Type::doubleTy());
}

std::string CryptoArgs::numberText(double value) const {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(int64_t(value));
  }

  return llvm::formatv("{0:g}", value).str();
}

std::optional<double> CryptoArgs::knownNumber(const Expr *arg) const {
  auto negations = std::vector<const Expr *>();
  const Expr *inner = arg;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    negations.push_back(inner);
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  auto value = this->plainNumber(inner);
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
  if (auto *literal = dyn_cast<FloatLitExpr>(arg)) {
    return literal->value;
  }
  if (auto *literal = dyn_cast<IntLitExpr>(arg)) {
    return double(literal->value);
  }
  auto *conversion = dyn_cast<CastExpr>(arg);
  if (conversion != nullptr) {
    if (std::optional<int64_t> value = this->constantFolder->intConstant(conversion->operand)) {
      return double(*value);
    }
  }
  auto value = this->constantFolder->intConstant(arg);
  if (!value) {
    return std::nullopt;
  }

  return double(*value);
}

bool CryptoArgs::checkWhole(const Expr *arg, llvm::StringRef name, double least) {
  auto value = this->knownNumber(arg);
  if (!value || (*value == std::floor(*value) && *value >= least)) {
    return true;
  }
  auto leastShown = this->numberText(least);
  auto valueShown = this->numberText(*value);
  auto message = ErrorTextCryptoNumber::whole(name, leastShown, valueShown);
  this->state.getDiagnostics().error(arg->loc(), message);

  return false;
}

bool CryptoArgs::checkCipherLengths(const Expr *cipher, const Expr *key, const Expr *iv) {
  auto *index = dyn_cast<IntLitExpr>(cipher);
  if (index == nullptr) {
    return true;
  }
  llvm::StringRef name = kDirectCiphers[index->value].name;
  int64_t keyBytes = name.contains("128") ? 16 : name.contains("192") ? 24 : 32;
  bool isAnyIv = name.ends_with("gcm");
  int64_t ivBytes = name.starts_with("chacha") ? 12 : 16;
  auto bytesOf = [&](const Expr *expr) -> std::optional<int64_t> {
    Type::Bytes kind = expr->type->bytes();
    if (kind != Type::Bytes::Buffer && kind != Type::Bytes::Uint8) {
      return std::nullopt;
    }

    return this->indexProofs->exactLength(expr);
  };
  bool isOk = true;
  std::optional<int64_t> have = bytesOf(key);
  if (have && *have != keyBytes) {
    auto message = ErrorTextCryptoCipher::keyBytes(name, keyBytes, *have);
    this->state.getDiagnostics().error(key->loc(), message);
    isOk = false;
  }
  std::optional<int64_t> ivHave = bytesOf(iv);
  if (ivHave && isAnyIv && *ivHave == 0) {
    auto message = ErrorTextCryptoCipher::emptyIv(name);
    this->state.getDiagnostics().error(iv->loc(), message);
    isOk = false;
  } else if (ivHave && !isAnyIv && *ivHave != ivBytes) {
    auto message = ErrorTextCryptoCipher::ivBytes(name, ivBytes, *ivHave);
    this->state.getDiagnostics().error(iv->loc(), message);
    isOk = false;
  }

  return isOk;
}

Expr *CryptoArgs::cryptoKey(Expr *arg, llvm::StringRef what) {
  return this->cryptoData(arg, what, true, true, true);
}

CryptoArgs::ResultKeyObjectType CryptoArgs::keyObjectType(const Type *type,
                                                          llvm::StringRef what) const {
  if (!type->isError() && type->crypto() != Crypto::KeyObject) {
    auto message = ErrorTextCryptoKey::keyObject(what, type->name());

    return err(CryptoArgsKeyObjectTypeError{
        .code = CryptoArgsKeyObjectTypeErrorCode::NOT_A_KEY_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

Expr *CryptoArgs::cryptoKeyObject(Expr *arg, llvm::StringRef what) {
  arg = this->exprChecker->checkExpr(arg);
  auto key = this->keyObjectType(arg->type, what);
  if (key.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), key.error().message);
    arg->type = Type::error();
  }

  return arg;
}

bool CryptoArgs::rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef functionName,
                                      llvm::StringRef hint) {
  if (index >= call->args.size()) {
    return false;
  }
  auto *arg = this->exprChecker->checkExpr(call->args[index]);
  if (!arg->type->nonOptional()->isFunction()) {
    return false;
  }
  auto message = ErrorTextCryptoKey::callback(functionName, hint);
  this->state.getDiagnostics().error(arg->loc(), message);
  for (Expr *&a : call->args) {
    if (a != call->args[index]) {
      a = this->exprChecker->checkExpr(a);
    }
  }
  call->type = Type::error();

  return true;
}

bool CryptoArgs::cryptoSignKey(Expr *arg, llvm::StringRef functionName, bool priv,
                               std::vector<Expr *> &args) {
  llvm::SMLoc loc = arg->loc();
  auto *padding = this->cryptoInt(loc, 0);
  auto *salt = this->cryptoInt(loc, 0);
  int p1363 = 0;
  int has = 0;
  Expr *key;
  if (isa<ObjectLitExpr>(arg)) {
    auto options = llvm::StringMap<Expr *>();
    if (!this->cryptoOptions(arg, functionName, {"key", "padding", "saltLength", "dsaEncoding"},
                             options)) {
      return false;
    }
    if (!options.lookup("key")) {
      auto message = ErrorTextCryptoOptions::needsKey(functionName);
      this->state.getDiagnostics().error(arg->loc(), message);
      return false;
    }
    auto keyWhat = ErrorTextCryptoLabel::key();
    key = this->cryptoKey(options["key"], keyWhat);
    Expr *p = options.lookup("padding");
    if (p != nullptr) {
      padding = this->argumentChecker->checkArg(p, Type::intTy());
      has |= 1;
    }
    Expr *s = options.lookup("saltLength");
    if (s != nullptr) {
      salt = this->argumentChecker->checkArg(s, Type::intTy());
      has |= 2;
    }
    Expr *d = options.lookup("dsaEncoding");
    if (d != nullptr) {
      auto signature = ErrorTextCryptoLabel::signature();
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
