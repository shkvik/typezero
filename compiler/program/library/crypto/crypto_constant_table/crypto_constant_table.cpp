#include "compiler/program/library/crypto/crypto_constant_table/crypto_constant_table.h"

namespace tz {

namespace {

const CryptoConstantInfo kConstants[] = {
    {.name = "RSA_PKCS1_PADDING", .value = 1},
    {.name = "RSA_NO_PADDING", .value = 3},
    {.name = "RSA_PKCS1_OAEP_PADDING", .value = 4},
    {.name = "RSA_X931_PADDING", .value = 5},
    {.name = "RSA_PKCS1_PSS_PADDING", .value = 6},
    {.name = "RSA_PSS_SALTLEN_DIGEST", .value = -1},
    {.name = "RSA_PSS_SALTLEN_MAX_SIGN", .value = -2},
    {.name = "RSA_PSS_SALTLEN_AUTO", .value = -2},
    {.name = "POINT_CONVERSION_COMPRESSED", .value = 2},
    {.name = "POINT_CONVERSION_UNCOMPRESSED", .value = 4},
    {.name = "POINT_CONVERSION_HYBRID", .value = 6},
};

}

std::optional<int64_t> CryptoConstantTable::find(llvm::StringRef name) {
  for (const CryptoConstantInfo &info : kConstants) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
