#include "compiler/link/licenses_pass/licenses_pass.h"

namespace tz {

CryptoUse LicensesPass::run(llvm::Module &module) const {
  CryptoUse crypto = Licenses::cryptoUse(module);
  Licenses::addLicenseManifest(
      module, {crypto.any, Licenses::usesEncodingIndexes(module), Licenses::usesHttp2(module)});

  return crypto;
}

}
