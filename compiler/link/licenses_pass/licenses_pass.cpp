#include "compiler/link/licenses_pass/licenses_pass.h"

namespace tz {

CryptoUse LicensesPass::run(llvm::Module &module, const std::vector<NativeLicense> &natives) const {
  CryptoUse crypto = Licenses::cryptoUse(module);
  Licenses::addLicenseManifest(module, {
                                           .openssl = crypto.any,
                                           .whatwg = Licenses::usesEncodingIndexes(module),
                                           .nghttp2 = Licenses::usesHttp2(module),
                                           .natives = natives,
                                       });

  return crypto;
}
}
