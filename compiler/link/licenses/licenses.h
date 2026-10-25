#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include <string>
#include <vector>

namespace tz {

// The code of others that goes into a program (docs/crypto.md, section 6): the parts of the
// runtime that follow Node.js and V8, always, and OpenSSL when the program uses node:crypto or
// https.
// `tz licenses app.exe` prints their licenses, which must go along with the exe.
struct Embedded {
  bool openssl = false;
  // The indexes of the WHATWG Encoding Standard: the program decodes a
  // multi-byte encoding of TextDecoder (runtime/library/bytes/cjk/cjk.c).
  bool whatwg = false;
};

// What of the runtime the module calls, by the names of its functions and constants: node:crypto
// in its direct layer (tz_crypto_*) or in full (tz_openssl_*), and TLS (tz_tls_*), which needs
// libssl and all of libcrypto (docs/net.md, section 4).
struct CryptoUse {
  bool any = false;
  bool full = false;
  bool tls = false;
};
CryptoUse cryptoUse(const llvm::Module &module);
// Whether the module names a multi-byte encoding of the runtime (tz_cjk_encoding_*), whose
// indexes come from the WHATWG Encoding Standard.
bool usesEncodingIndexes(const llvm::Module &module);

// Writes what the program embeds into it, in a section `tz licenses` reads.
void addLicenseManifest(llvm::Module &module, const Embedded &embedded);

// `tz licenses <exe>`: prints the licenses of what the exe embeds, with the texts from the
// directory `licenses` next to tz. Returns the exit code.
int printLicenses(llvm::StringRef exe, llvm::StringRef licensesDir);

} // namespace tz
