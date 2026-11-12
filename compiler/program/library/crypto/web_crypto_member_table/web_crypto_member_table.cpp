#include "compiler/program/library/crypto/web_crypto_member_table/web_crypto_member_table.h"

namespace tz {

namespace {

const WebCryptoMemberInfo kRows[] = {
    {
        .name = "randomUUID",
        .value = WebCryptoMember::RANDOM_UUID,
    },
    {
        .name = "getRandomValues",
        .value = WebCryptoMember::GET_RANDOM_VALUES,
    },
    {
        .name = "subtle",
        .value = WebCryptoMember::SUBTLE,
    },
};

}

std::optional<WebCryptoMember> WebCryptoMemberTable::find(llvm::StringRef name) {
  for (const WebCryptoMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
