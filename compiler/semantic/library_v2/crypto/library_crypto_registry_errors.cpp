#include "compiler/semantic/library_v2/crypto/library_crypto_registry_errors.h"
#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"

namespace tz {

std::string ErrorTextCryptoData::notData(llvm::StringRef what, bool strings, bool buffers,
                                         bool keys, const Type *type) {
  std::string kinds = strings ? "a string or bytes" : "bytes";

  return "the " + what.str() + " is " + kinds + " (a Buffer, a typed array or a DataView" +
         (buffers ? ", an ArrayBuffer" : "") + (keys ? ", a KeyObject" : "") + "); found '" +
         type->name().str() + "'" + (type->isNumeric() ? "; make text of it with 'String(x)'" : "");
}

std::string ErrorTextCryptoData::encoding(llvm::StringRef fn) {
  return "the encoding of '" + fn.str() +
         "' is written as a literal, e.g. 'hex': "
         "the type of what it gives depends on it";
}

std::string ErrorTextCryptoOptions::literal(llvm::StringRef fn, llvm::StringRef first) {
  return "the options of '" + fn.str() +
         "' are written in the call, as an object literal, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextCryptoOptions::byName(llvm::StringRef fn, llvm::StringRef first) {
  return "write each option of '" + fn.str() + "' by its name, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextCryptoOptions::unknown(llvm::StringRef fn, llvm::StringRef name,
                                            llvm::ArrayRef<llvm::StringRef> allowed) {
  auto list = std::string();
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }

  return "'" + fn.str() + "' has no option '" + name.str() + "'; it takes " + list;
}

std::string ErrorTextCryptoOptions::needsKey(llvm::StringRef fn) {
  return "the options of '" + fn.str() + "' need 'key'";
}

std::string ErrorTextCryptoChoice::list(llvm::ArrayRef<llvm::StringRef> values) {
  auto list = std::string();
  for (size_t i = 0; i < values.size(); ++i) {
    list += (i == 0 ? "'" : i + 1 == values.size() ? "' or '" : "', '") + values[i].str();
  }

  return list + "'";
}

std::string ErrorTextCryptoChoice::literal(llvm::StringRef what,
                                           llvm::ArrayRef<llvm::StringRef> values) {
  return "the " + what.str() + " is written as a literal: " + ErrorTextCryptoChoice::list(values);
}

std::string ErrorTextCryptoChoice::unknown(llvm::StringRef what, llvm::StringRef text,
                                           llvm::ArrayRef<llvm::StringRef> values) {
  return "unknown " + what.str() + " '" + text.str() + "'; it is " +
         ErrorTextCryptoChoice::list(values);
}

std::string ErrorTextCryptoHash::anyLength(llvm::StringRef text) {
  return "'" + text.str() +
         "' gives output of any length, which HMAC and the "
         "KDFs cannot use; use a hash such as 'sha256'";
}

std::string ErrorTextCryptoHash::turnedOff(llvm::StringRef text) {
  return "'" + text.str() +
         "' is turned off in node 24 (it throws "
         "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'sha256'";
}

std::string ErrorTextCryptoHash::unknown(llvm::StringRef text) {
  return "unknown hash '" + text.str() +
         "'; node has those getHashes() lists, e.g. "
         "'sha256', 'sha512', 'sha3-256', 'md5'";
}

std::string ErrorTextCryptoCipher::turnedOff(llvm::StringRef text) {
  return "'" + text.str() +
         "' is turned off in node 24 (it throws "
         "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use "
         "'aes-256-gcm'";
}

std::string ErrorTextCryptoCipher::unknown(llvm::StringRef text) {
  return "unknown cipher '" + text.str() +
         "'; node has those getCiphers() lists, "
         "e.g. 'aes-256-gcm', 'aes-256-cbc', "
         "'chacha20-poly1305'";
}

std::string ErrorTextCryptoCipher::keyBytes(llvm::StringRef name, int64_t want, int64_t have) {
  return name.str() + " needs a " + std::to_string(want) + "-byte key, but this one has " +
         std::to_string(have) + " bytes";
}

std::string ErrorTextCryptoCipher::emptyIv(llvm::StringRef name) {
  return name.str() + " needs an IV of 1 byte or more, but it is empty";
}

std::string ErrorTextCryptoCipher::ivBytes(llvm::StringRef name, int64_t want, int64_t have) {
  return name.str() + " needs a " + std::to_string(want) + "-byte IV, but this one has " +
         std::to_string(have) + " bytes";
}

std::string ErrorTextCryptoNumber::whole(llvm::StringRef name, llvm::StringRef least,
                                         llvm::StringRef value) {
  return name.str() + " must be a whole number of " + least.str() + " or more, not " + value.str();
}

std::string ErrorTextCryptoKey::keyObject(llvm::StringRef what, llvm::StringRef type) {
  return "the " + what.str() + " is a KeyObject; found '" + type.str() + "'";
}

std::string ErrorTextCryptoKey::callback(llvm::StringRef fn, llvm::StringRef hint) {
  return "'" + fn.str() +
         "' with a callback is not supported: tz has no callback forms "
         "of node:crypto (docs/crypto.md, section 5); call it without one "
         "and use what it returns: " +
         hint.str() + ", or run it on the thread pool with '" + fn.str() +
         "Async' of \"tz:crypto\": 'await " + fn.str() + "Async(...)'";
}

std::string ErrorTextCryptoLabel::key() {
  return "key";
}

std::string ErrorTextCryptoLabel::signatureEncoding() {
  return "encoding of the signature";
}

std::string ErrorTextCryptoLabel::privateKey() {
  return "private key";
}

std::string ErrorTextCryptoLabel::publicKey() {
  return "public key";
}

std::string ErrorTextCryptoLabel::otherKey() {
  return "other key";
}

std::string ErrorTextCryptoLabel::signature() {
  return "signature";
}

std::string ErrorTextCryptoLabel::data() {
  return "data";
}

std::string ErrorTextCryptoLabel::password() {
  return "password";
}

std::string ErrorTextCryptoLabel::salt() {
  return "salt";
}

std::string ErrorTextCryptoLabel::iterations() {
  return "iterations";
}

std::string ErrorTextCryptoLabel::keylen() {
  return "keylen";
}

std::string ErrorTextCryptoLabel::ikm() {
  return "ikm";
}

std::string ErrorTextCryptoLabel::info() {
  return "info";
}

std::string ErrorTextCryptoLabel::iv() {
  return "iv";
}

std::string ErrorTextCryptoLabel::candidate() {
  return "candidate";
}

std::string ErrorTextCryptoLabel::size() {
  return "size";
}

std::string ErrorTextCryptoLabel::aad() {
  return "AAD";
}

std::string ErrorTextCryptoLabel::tag() {
  return "tag";
}

std::string ErrorTextCryptoLabel::subjectOption() {
  return "subject option";
}

std::string ErrorTextCryptoMethods::of(Type::Crypto kind) {
  switch (kind) {
    case Type::Crypto::Hash:
      return "'update', 'digest' and 'copy'";
    case Type::Crypto::Hmac:
      return "'update' and 'digest'";
    case Type::Crypto::Cipheriv:
      return "'update', 'final', 'setAAD', 'getAuthTag' and 'setAutoPadding'";
    case Type::Crypto::Decipheriv:
      return "'update', 'final', 'setAAD', 'setAuthTag' and 'setAutoPadding'";
    case Type::Crypto::KeyObject:
      return "'export' and 'equals', and the properties 'type', 'asymmetricKeyType' and "
             "'symmetricKeySize'";
    case Type::Crypto::Sign:
      return "'update' and 'sign'";
    case Type::Crypto::Verify:
      return "'update' and 'verify'";
    case Type::Crypto::Ecdh:
      return "'generateKeys', 'computeSecret', 'getPublicKey', 'getPrivateKey' and "
             "'setPrivateKey'";
    case Type::Crypto::X509:
      return "'checkHost', 'checkEmail', 'checkIP', 'checkIssued', 'checkPrivateKey', 'verify', "
             "'toString' and 'toJSON', and the properties 'subject', 'issuer', 'subjectAltName', "
             "'infoAccess', 'validFrom', 'validTo', 'validFromDate', 'validToDate', "
             "'serialNumber', 'fingerprint', 'fingerprint256', 'fingerprint512', 'keyUsage', "
             "'ca', 'raw' and 'publicKey'";
    case Type::Crypto::None:
      break;
  }

  return "";
}

std::string ErrorTextCryptoValue::constants() {
  return "'constants' of 'node:crypto' is used as "
         "'constants.RSA_PKCS1_PSS_PADDING' and the like";
}

std::string ErrorTextCryptoValue::isType(llvm::StringRef name) {
  return "'" + name.str() +
         "' of 'node:crypto' is a type: its objects come from "
         "the functions of 'node:crypto'";
}

std::string ErrorTextCryptoValue::constant(llvm::StringRef name) {
  return "'constants." + name.str() +
         "' of 'node:crypto' is not supported yet; tz has the RSA_ and "
         "POINT_CONVERSION_ ones";
}

std::string ErrorTextCryptoMember::keyDetails() {
  return "'asymmetricKeyDetails' is not supported yet: node gives the exponent of "
         "RSA as a bigint, which tz has not";
}

std::string ErrorTextCryptoMember::issuerCert() {
  return "'issuerCertificate' comes only with TLS (docs/net.md)";
}

std::string ErrorTextCryptoMember::onlyCalled(llvm::StringRef name, llvm::StringRef type,
                                              llvm::StringRef methods) {
  return "property '" + name.str() + "' of " + type.str() + " can only be called; it has " +
         methods.str();
}

std::string ErrorTextCryptoMake::notNode(llvm::StringRef cls) {
  return "'new " + cls.str() + "' is not how node makes one: " +
         (cls == "KeyObject"                 ? "use 'createSecretKey', 'createPublicKey' "
                                               "or 'createPrivateKey'"
          : cls == "ECDH"                    ? "use 'createECDH(curve)'"
          : cls == "Sign" || cls == "Verify" ? "use 'create" + cls.str() + "(algorithm)'"
          : cls == "Hash" || cls == "Hmac"   ? "use 'create" + cls.str() + "(algorithm)'"
                                             : "use 'create" + cls.str() + "(algorithm, key, iv)'");
}

std::string ErrorTextCryptoMake::x509() {
  return "'new X509Certificate' may fail on the data it reads, and 'new' gives no "
         "Result; write 'X509Certificate.parse(pem)', which gives a "
         "Result<X509Certificate, ParseCertificateError>";
}

std::string ErrorTextX509::noStatic(llvm::StringRef name) {
  return "'X509Certificate." + name.str() +
         "' does not exist; read a certificate with "
         "'X509Certificate.parse(pem)'";
}

std::string ErrorTextX509::parseArgs() {
  return "'X509Certificate.parse' takes the certificate, PEM or DER";
}

std::string ErrorTextX509::data() {
  return "certificate";
}

std::string ErrorTextGlobalCrypto::subtleCall() {
  return "'crypto.subtle' is an object, not a function";
}

std::string ErrorTextGlobalCrypto::noMember(llvm::StringRef name) {
  return "'crypto." + name.str() +
         "' does not exist; the global crypto has "
         "'randomUUID', 'getRandomValues' and "
         "'subtle'";
}

std::string ErrorTextGlobalCrypto::onlyCalled(llvm::StringRef name) {
  return "'crypto." + name.str() + "' can only be called";
}

std::string ErrorTextCryptoFunction::callback(llvm::StringRef fn) {
  return "'" + fn.str() +
         "' with a callback is not supported: tz has no callbacks "
         "(docs/crypto.md, section 5); without one it returns a Promise: "
         "'const result = await " +
         fn.str() + "(...)'";
}

std::string ErrorTextCryptoFunction::isClass(llvm::StringRef fn) {
  return "'" + fn.str() + "' of 'node:crypto' is a class, not a function" +
         (fn == "X509Certificate" ? "; make one with 'X509Certificate.parse(pem)'" : "");
}

std::string ErrorTextCryptoFunction::randomArgs() {
  return "'randomInt' takes a minimum and a maximum";
}

std::string ErrorTextCryptoFunction::randomInt(llvm::StringRef type) {
  return "'randomInt' takes whole numbers ('int'), found '" + type.str() + "'";
}

std::string ErrorTextCryptoFunction::greater(bool minimum) {
  return minimum ? "the maximum is greater than the minimum" : "the maximum is greater than 0";
}

std::string ErrorTextCryptoFunction::randomVals(llvm::StringRef type) {
  return "'getRandomValues' fills an array of integers (a Uint8Array, "
         "an Int32Array, a Buffer...); found '" +
         type.str() + "'";
}

std::string ErrorTextCryptoFunction::fillsBytes(llvm::StringRef shown, llvm::StringRef type) {
  return "'" + shown.str() +
         "' fills bytes (a Buffer, a typed array, a "
         "DataView or an ArrayBuffer); found '" +
         type.str() + "'";
}

std::string ErrorTextCryptoFunction::fillFits(llvm::StringRef count, llvm::StringRef at) {
  return "cannot prove that the offset and the size fit in the bytes, "
         "and JavaScript would throw a RangeError; check it first, e.g. "
         "'if (" +
         count.str() + " <= buf.length - " + at.str() + ")'";
}

std::string ErrorTextCryptoFunction::together(llvm::StringRef first, llvm::StringRef second) {
  return "option '" + first.str() + "' cannot be used together with '" + second.str() +
         "': they are one option";
}

std::string ErrorTextCryptoFunction::costN(llvm::StringRef cost) {
  return "N must be a power of two greater than 1, not " + cost.str();
}

std::string ErrorTextCryptoFunction::needs(llvm::StringRef shown, llvm::StringRef required) {
  return "the parameters of '" + shown.str() + "' need '" + required.str() + "'";
}

std::string ErrorTextCryptoFunction::tagLength(llvm::StringRef name) {
  return "'" + name.str() +
         "' needs the length of its tag: '{ "
         "authTagLength: 16 }'";
}

std::string ErrorTextCryptoFunction::keyCoding() {
  return "the encoding of the key is written as a literal, e.g. 'hex'";
}

std::string ErrorTextCryptoFunction::keyLength(llvm::StringRef shown) {
  return "'" + shown.str() +
         "' needs the length of the key in bits: '{ "
         "length: 256 }'";
}

std::string ErrorTextCryptoFunction::aesLength() {
  return "the length of an AES key is 128, 192 or 256, written "
         "as a number; JavaScript throws a TypeError otherwise";
}

std::string ErrorTextCryptoFunction::curve(llvm::StringRef curve) {
  return "unknown curve '" + curve.str() +
         "'; node has those getCurves() lists, e.g. "
         "'prime256v1', 'secp384r1', 'secp256k1'";
}

std::string ErrorTextCryptoFunction::dhKeys() {
  return "'diffieHellman' needs '{ privateKey, publicKey }'";
}

std::string ErrorTextCryptoFunction::bigint() {
  return "'bigint' is not supported: tz has no bigint "
         "(docs/numbers.md); the prime comes as an ArrayBuffer";
}

std::string ErrorTextCryptoFnLabel::offset() {
  return "the offset";
}

std::string ErrorTextCryptoFnLabel::size() {
  return "the size";
}

std::string ErrorTextCryptoFnLabel::keyBits() {
  return "the length of the key";
}

std::string ErrorTextCryptoFnLabel::first() {
  return "first argument";
}

std::string ErrorTextCryptoFnLabel::second() {
  return "second argument";
}

std::string ErrorTextCryptoFnLabel::argon() {
  return "algorithm of Argon2";
}

std::string ErrorTextCryptoFnLabel::keyFormat() {
  return "format of the key";
}

std::string ErrorTextCryptoFnLabel::keyType() {
  return "type of the key";
}

std::string ErrorTextCryptoFnLabel::privateKey() {
  return "private key";
}

std::string ErrorTextCryptoFnLabel::publicKey() {
  return "public key";
}

std::string ErrorTextCryptoPoint::format() {
  return "format of the point";
}

std::string ErrorTextCryptoMethod::issuer(llvm::StringRef type) {
  return "'checkIssued' takes an X509Certificate; found '" + type.str() + "'";
}

std::string ErrorTextCryptoMethod::stream(llvm::StringRef name, llvm::StringRef type,
                                          Type::Crypto kind, bool cipher) {
  const char *instead = kind == Type::Crypto::Sign ? "'update(data)' and 'sign(key)'"
                        : kind == Type::Crypto::Verify
                            ? "'update(data)' and 'verify(key, signature)'"
                        : cipher ? "'update(data)' and 'final()'"
                                 : "'update(data)' and 'digest()'";

  return "'" + name.str() + "' of " + type.str() +
         " is not supported yet: in node it is a stream too, and tz has no "
         "streams yet (docs/streams.md); use " +
         instead;
}

std::string ErrorTextCryptoMethod::noMethod(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef methods) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'; it " + "has " +
         methods.str();
}

std::string ErrorTextKeyPair::option(llvm::StringRef name) {
  return "the option '" + name.str() +
         "' of 'rsa-pss' keys is not "
         "supported yet";
}

std::string ErrorTextKeyPair::modulus(llvm::StringRef type) {
  return "'" + type.str() + "' keys need '{ modulusLength: 2048 }'";
}

std::string ErrorTextKeyPair::curve() {
  return "'ec' keys need '{ namedCurve: \"P-256\" }'";
}

std::string ErrorTextKeyPair::small(llvm::StringRef bits) {
  return "an RSA key needs a modulusLength of 512 bits or more, not " + bits.str();
}

std::string ErrorTextKeyPair::noFormat(llvm::StringRef option) {
  return "'" + option.str() + "' needs 'format': 'pem', 'der' or 'jwk'";
}

std::string ErrorTextKeyPair::noType(llvm::StringRef option, bool publicKey) {
  return "'" + option.str() +
         "' needs 'type': " + (publicKey ? "'spki' or 'pkcs1'" : "'pkcs8', 'pkcs1' or 'sec1'");
}

std::string ErrorTextKeyPair::passphrase() {
  return "a cipher needs a 'passphrase'";
}

std::string ErrorTextKeyPairLabel::pairType() {
  return "type of the key pair";
}

std::string ErrorTextKeyPairLabel::format() {
  return "format of the key";
}

std::string ErrorTextKeyPairLabel::keyType() {
  return "type of the key";
}

std::string ErrorTextKeyFormat::unknown(llvm::StringRef format) {
  return "unknown key format '" + format.str() + "'; it is 'raw', 'pkcs8', 'spki' or 'jwk'";
}

std::string ErrorTextSubtleUsage::unknown(llvm::StringRef usage) {
  return "usage '" + usage.str() + "' does not exist; the usages are " +
         SubtleAlgorithms::quotedList(SubtleKeyUsageTable::getNames(), "and");
}

std::string ErrorTextSubtleUsage::notOf(llvm::StringRef usage, llvm::StringRef name, int isPublic,
                                        bool isSecret, llvm::ArrayRef<llvm::StringRef> allowed) {
  std::string an = llvm::StringRef("EHRAX").contains(name.front()) ? "an " : "a ";
  std::string what = isPublic == 1                ? "a public " + name.str() + " key"
                     : isPublic == 0 && !isSecret ? "a private " + name.str() + " key"
                                                  : an + name.str() + " key";
  std::string takes = allowed.empty() ? std::string("it takes none: pass []")
                                      : "it takes " + SubtleAlgorithms::quotedList(allowed, "and");

  return "'" + usage.str() + "' is no usage of " + what + "; " + takes;
}

std::string ErrorTextSubtleUsage::empty(llvm::ArrayRef<llvm::StringRef> own) {
  return "a key without usages can do nothing, and node rejects it "
         "('Usages cannot be empty'); give its usages, e.g. " +
         SubtleAlgorithms::quotedList(own, "and");
}

std::string ErrorTextSubtleUsage::pairEmpty(llvm::ArrayRef<llvm::StringRef> own) {
  return "the private key of the pair would have no usages, which node "
         "rejects ('Usages cannot be empty'); add " +
         SubtleAlgorithms::quotedList(own, "or");
}

std::string ErrorTextSubtleKeyData::jwk(llvm::StringRef type) {
  return "the key data of 'jwk' is a JsonWebKey; found '" + type.str() + "'";
}

std::string ErrorTextSubtleKeyData::bytes(llvm::StringRef format, llvm::StringRef type) {
  return "the key data of '" + format.str() + "' is bytes (a BufferSource); found '" + type.str() +
         "'";
}

std::string ErrorTextSubtleImport::format(llvm::StringRef name, llvm::StringRef format,
                                          llvm::ArrayRef<llvm::StringRef> formats) {
  return "'" + name.str() + "' keys are not imported as '" + format.str() +
         "' (node: 'Unable to import " + name.str() + " using " + format.str() +
         " format'); it takes " + SubtleAlgorithms::quotedList(formats, "or");
}

std::string ErrorTextSubtleImport::unwrap(llvm::StringRef name, llvm::StringRef format,
                                          llvm::ArrayRef<llvm::StringRef> formats) {
  return "'" + name.str() + "' keys are not imported as '" + format.str() + "'; it takes " +
         SubtleAlgorithms::quotedList(formats, "or");
}

std::string ErrorTextSubtleImport::derived(llvm::StringRef name) {
  return "deriveKey makes secret keys: AES, HMAC, HKDF or PBKDF2, not '" + name.str() + "'";
}

std::string ErrorTextAlgorithm::verb(SubtleOperation operation) {
  switch (operation) {
    case SubtleOperation::DIGEST:
      return "hashes";
    case SubtleOperation::GENERATE_KEY:
      return "makes keys";
    case SubtleOperation::IMPORT_KEY:
      return "imports keys";
    case SubtleOperation::ENCRYPT:
      return "encrypts";
    case SubtleOperation::DECRYPT:
      return "decrypts";
    case SubtleOperation::SIGN:
      return "signs";
    case SubtleOperation::VERIFY:
      return "verifies";
    case SubtleOperation::DERIVE_BITS:
      return "derives";
    case SubtleOperation::WRAP_KEY:
      return "wraps keys";
    case SubtleOperation::UNWRAP_KEY:
      return "unwraps keys";
    case SubtleOperation::KEY_LENGTH:
      break;
  }

  return "derives keys";
}

std::string ErrorTextAlgorithm::choices(llvm::ArrayRef<llvm::StringRef> names) {
  return SubtleAlgorithms::quotedList(names, "or");
}

std::string ErrorTextAlgorithm::experiment(llvm::StringRef written, llvm::StringRef verb,
                                           llvm::StringRef list) {
  return "'" + written.str() +
         "' is an algorithm node 24 has as an experiment, which tz has not "
         "yet; Web Crypto " +
         verb.str() + " with " + list.str();
}

std::string ErrorTextAlgorithm::otherOp(llvm::StringRef written, llvm::StringRef op,
                                        llvm::StringRef verb, llvm::StringRef list) {
  return "'" + written.str() + "' is no algorithm of " + op.str() + "; it " + verb.str() +
         " with " + list.str();
}

std::string ErrorTextAlgorithm::unknown(llvm::StringRef written, llvm::StringRef op,
                                        llvm::StringRef verb, llvm::StringRef list) {
  return "unknown algorithm '" + written.str() + "' of " + op.str() + "; Web Crypto " + verb.str() +
         " with " + list.str();
}

std::string ErrorTextAlgorithm::object(llvm::StringRef name, llvm::StringRef op,
                                       llvm::StringRef dictionary, llvm::StringRef member,
                                       llvm::StringRef example) {
  return "'" + name.str() + "' of " + op.str() + " takes its parameters (" + dictionary.str() +
         "): write an object, e.g. { name: \"" + name.str() + "\", " + member.str() + ": " +
         example.str() + " }";
}

std::string ErrorTextAlgorithm::needs(llvm::StringRef name, llvm::StringRef op,
                                      llvm::StringRef member, llvm::StringRef dictionary,
                                      llvm::StringRef example) {
  return "the algorithm '" + name.str() + "' of " + op.str() + " needs '" + member.str() + "' (" +
         dictionary.str() + "); add it, e.g. '" + member.str() + ": " + example.str() + "'";
}

std::string ErrorTextAlgorithm::hash(llvm::StringRef hash, llvm::StringRef dictionary,
                                     llvm::StringRef list) {
  return "unknown hash '" + hash.str() + "' of " + dictionary.str() + "; Web Crypto hashes with " +
         list.str();
}

std::string ErrorTextAlgorithm::notSha(llvm::StringRef hash, llvm::StringRef dictionary) {
  return "'" + hash.str() + "' is no SHA hash, which " + dictionary.str() +
         " takes; use 'SHA-256', 'SHA-384', 'SHA-512' or 'SHA-1'";
}

std::string ErrorTextAlgorithm::curve(llvm::StringRef curve) {
  return "unknown namedCurve '" + curve.str() + "'; Web Crypto has 'P-256', 'P-384' and 'P-521'";
}

std::string ErrorTextAlgorithm::aesLength() {
  return "an AES key is 128, 192 or 256 bits long";
}

std::string ErrorTextAlgorithm::gcmTag() {
  return "the tag of AES-GCM is 32, 64, 96, 104, 112, 120 or 128 "
         "bits long";
}

std::string ErrorTextSubtle::literal(llvm::StringRef name) {
  return "the hash or the cipher of '" + name.str() + "' is a literal of the direct layer";
}

std::string ErrorTextSubtle::noSubtle() {
  return "'crypto.subtle' is not available here";
}

}
