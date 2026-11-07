#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include <string>
#include <vector>

namespace tz {

struct Embedded {
  bool openssl = false;
  bool whatwg = false;
  bool nghttp2 = false;
};

struct CryptoUse {
  bool any = false;
  bool full = false;
  bool tls = false;
};

class Licenses {
public:
  static CryptoUse cryptoUse(const llvm::Module &module);
  static bool usesEncodingIndexes(const llvm::Module &module);
  static bool usesHttp2(const llvm::Module &module);
  static void addLicenseManifest(llvm::Module &module, const Embedded &embedded);
  static int printLicenses(llvm::StringRef exe, llvm::StringRef licensesDir);
};

}
