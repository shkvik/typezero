#include "compiler/program/library/crypto/crypto_export_table/crypto_export_table.h"

namespace tz {

namespace {

const CryptoExportInfo kExports[] = {
    {.name = "webcrypto", .kind = CryptoExport::WEBCRYPTO},
    {.name = "subtle", .kind = CryptoExport::SUBTLE},
    {.name = "Certificate", .kind = CryptoExport::NOT_YET},
    {.name = "DiffieHellman", .kind = CryptoExport::NOT_YET},
    {.name = "DiffieHellmanGroup", .kind = CryptoExport::NOT_YET},
    {.name = "createDiffieHellman", .kind = CryptoExport::NOT_YET},
    {.name = "createDiffieHellmanGroup", .kind = CryptoExport::NOT_YET},
    {.name = "decapsulate", .kind = CryptoExport::NOT_YET},
    {.name = "encapsulate", .kind = CryptoExport::NOT_YET},
    {.name = "getCipherInfo", .kind = CryptoExport::NOT_YET},
    {.name = "getDiffieHellman", .kind = CryptoExport::NOT_YET},
    {.name = "getFips", .kind = CryptoExport::NOT_YET},
    {.name = "privateDecrypt", .kind = CryptoExport::NOT_YET},
    {.name = "privateEncrypt", .kind = CryptoExport::NOT_YET},
    {.name = "publicDecrypt", .kind = CryptoExport::NOT_YET},
    {.name = "publicEncrypt", .kind = CryptoExport::NOT_YET},
    {.name = "secureHeapUsed", .kind = CryptoExport::NOT_YET},
    {.name = "setEngine", .kind = CryptoExport::NOT_YET},
    {.name = "setFips", .kind = CryptoExport::NOT_YET},
};

}

std::optional<CryptoExport> CryptoExportTable::find(llvm::StringRef name) {
  for (const CryptoExportInfo &info : kExports) {
    if (info.name == name) {
      return info.kind;
    }
  }

  return std::nullopt;
}

}
