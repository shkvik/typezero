#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker_errors.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"

namespace tz {

std::string ErrorTextKeyFormat::unknown(llvm::StringRef format) {
  return "unknown key format '" + format.str() + "'; it is 'raw', 'pkcs8', 'spki' or 'jwk'";
}

std::string ErrorTextSubtleUsage::unknown(llvm::StringRef usage) {
  return "usage '" + usage.str() + "' does not exist; the usages are " +
         SubtleAlgorithms::quotedList(kUsages, "and");
}

std::string ErrorTextSubtleUsage::notOf(llvm::StringRef usage, llvm::StringRef name, int isPublic,
                                        llvm::ArrayRef<llvm::StringLiteral> allowed) {
  std::string an = llvm::StringRef("EHRAX").contains(name.front()) ? "an " : "a ";
  std::string what = isPublic == 1 ? "a public " + name.str() + " key"
                     : isPublic == 0 && name != "HMAC" &&
                             SubtleAlgorithms::familyOf(name) != "Aes" &&
                             SubtleAlgorithms::familyOf(name) != "Kdf"
                         ? "a private " + name.str() + " key"
                         : an + name.str() + " key";
  std::string takes = allowed.empty() ? std::string("it takes none: pass []")
                                      : "it takes " + SubtleAlgorithms::quotedList(allowed, "and");

  return "'" + usage.str() + "' is no usage of " + what + "; " + takes;
}

std::string ErrorTextSubtleUsage::empty(llvm::ArrayRef<llvm::StringLiteral> own) {
  return "a key without usages can do nothing, and node rejects it "
         "('Usages cannot be empty'); give its usages, e.g. " +
         SubtleAlgorithms::quotedList(own, "and");
}

std::string ErrorTextSubtleUsage::pairEmpty(llvm::ArrayRef<llvm::StringLiteral> own) {
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
                                          llvm::ArrayRef<llvm::StringLiteral> formats) {
  return "'" + name.str() + "' keys are not imported as '" + format.str() +
         "' (node: 'Unable to import " + name.str() + " using " + format.str() +
         " format'); it takes " + SubtleAlgorithms::quotedList(formats, "or");
}

std::string ErrorTextSubtleImport::unwrap(llvm::StringRef name, llvm::StringRef format,
                                          llvm::ArrayRef<llvm::StringLiteral> formats) {
  return "'" + name.str() + "' keys are not imported as '" + format.str() + "'; it takes " +
         SubtleAlgorithms::quotedList(formats, "or");
}

std::string ErrorTextSubtleImport::derived(llvm::StringRef name) {
  return "deriveKey makes secret keys: AES, HMAC, HKDF or PBKDF2, not '" + name.str() + "'";
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

}
