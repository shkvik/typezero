#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class CryptoExport {
  WEBCRYPTO,
  SUBTLE,
  NOT_YET,
};

struct CryptoExportInfo {
  llvm::StringLiteral name;
  CryptoExport kind;
};

class CryptoExportTable {
public:
  static std::optional<CryptoExport> find(llvm::StringRef name);
};

}
