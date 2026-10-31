#include "compiler/link/licenses_pass/licenses_pass.h"

namespace tz {

CryptoUse LicensesPass::run(llvm::Module &module) const {
  CryptoUse crypto = cryptoUse(module);
  addLicenseManifest(module, {crypto.any, usesEncodingIndexes(module), usesHttp2(module)});

  return crypto;
}

}
