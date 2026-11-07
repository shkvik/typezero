#include "compiler/pipeline/check_program/topics/crypto/crypto_function_checker/crypto_function_checker_errors.h"

namespace tz {

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

}
