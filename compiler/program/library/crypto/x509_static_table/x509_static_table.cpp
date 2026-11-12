#include "compiler/program/library/crypto/x509_static_table/x509_static_table.h"

namespace tz {

namespace {

const X509StaticInfo kRows[] = {
    {
        .name = "parse",
        .value = X509Static::PARSE,
    },
};

}

std::optional<X509Static> X509StaticTable::find(llvm::StringRef name) {
  for (const X509StaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
