#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

namespace tz {

// The algorithms of Web Crypto: their names, parameters, usages and formats.

// A string written as a literal.
std::optional<std::string> stringLiteral(const Expr *e);

// What node 24 registers for each operation, as library/crypto/webcrypto.tz has them.
const std::vector<llvm::StringLiteral> &algorithmsOf(llvm::StringRef op);

// The algorithms node 24 has as experiments, which tz has not yet.
bool experimentalAlgorithm(llvm::StringRef upper);

std::string quotedList(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef last);

// The family of an algorithm: what makes and uses its keys (library/crypto/webcrypto.tz).
llvm::StringRef familyOf(llvm::StringRef name);

// The usages a key of `name` takes: of its public key, of its private (or secret) one.
std::vector<llvm::StringLiteral> usagesOf(llvm::StringRef name, bool isPublic);

// The formats importKey takes for a family.
std::vector<llvm::StringLiteral> formatsOf(llvm::StringRef family);

// The dictionary of Web IDL and its members a call must give, for an algorithm of an
// operation; empty for none.
struct Dictionary {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> required;
};

Dictionary dictionaryOf(llvm::StringRef op, llvm::StringRef name);

// How an operation is said in a message.
std::string verbOf(llvm::StringRef op);

// A value of a member of a dictionary, for the hint of an error.
std::string exampleOf(llvm::StringRef member);

inline const llvm::StringLiteral kUsages[] = {"encrypt",   "decrypt",    "sign",    "verify",
                                              "deriveKey", "deriveBits", "wrapKey", "unwrapKey"};

// An algorithm written in the call: its canonical name, or "" when the call does not write one
// (or after an error, `bad`). `op` is what node normalizes it for; `fallback` the operation
// node tries after it (encrypt for wrapKey).
std::string literalAlgorithm(Diagnostics &diags, const Expr *arg, llvm::StringRef op,
                             llvm::StringRef fallback, bool &bad);

} // namespace tz
