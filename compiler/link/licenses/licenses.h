#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include <string>
#include <vector>

namespace tz {

struct Embedded {
  bool openssl = false;
  bool whatwg = false;
};

struct CryptoUse {
  bool any = false;
  bool full = false;
  bool tls = false;
};
CryptoUse cryptoUse(const llvm::Module &module);
bool usesEncodingIndexes(const llvm::Module &module);

void addLicenseManifest(llvm::Module &module, const Embedded &embedded);

int printLicenses(llvm::StringRef exe, llvm::StringRef licensesDir);

}
