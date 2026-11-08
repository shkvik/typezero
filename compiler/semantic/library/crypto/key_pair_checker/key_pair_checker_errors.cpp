#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker_errors.h"

namespace tz {

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

}
