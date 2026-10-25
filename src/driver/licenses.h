#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"

#include <string>
#include <vector>

namespace tz {

// The code of others that goes into a program (docs/crypto.md, section 6): the parts of the
// runtime that follow Node.js and V8, always, and OpenSSL when the program uses node:crypto.
// `tz licenses app.exe` prints their licenses, which must go along with the exe.
struct Embedded {
  bool openssl = false;
};

// What of the runtime the module calls, by the names of its functions and constants: node:crypto
// in its direct layer (tz_crypto_*) or in full (tz_openssl_*).
struct CryptoUse {
  bool any = false;
  bool full = false;
};
CryptoUse cryptoUse(const llvm::Module &module);

// Writes what the program embeds into it, in a section `tz licenses` reads.
void addLicenseManifest(llvm::Module &module, const Embedded &embedded);

// `tz licenses <exe>`: prints the licenses of what the exe embeds, with the texts from the
// directory `licenses` next to tz. Returns the exit code.
int printLicenses(llvm::StringRef exe, llvm::StringRef licensesDir);

} // namespace tz
