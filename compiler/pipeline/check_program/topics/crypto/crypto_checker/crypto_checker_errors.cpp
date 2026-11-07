#include "compiler/pipeline/check_program/topics/crypto/crypto_checker/crypto_checker_errors.h"

namespace tz {

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

}
