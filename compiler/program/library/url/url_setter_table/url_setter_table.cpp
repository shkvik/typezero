#include "compiler/program/library/url/url_setter_table/url_setter_table.h"

namespace tz {

namespace {

const UrlSetterInfo kRows[] = {
    {
        .name = "setHref",
        .value = UrlSetter::HREF,
    },
    {
        .name = "setProtocol",
        .value = UrlSetter::PROTOCOL,
    },
    {
        .name = "setHost",
        .value = UrlSetter::HOST,
    },
    {
        .name = "setHostname",
        .value = UrlSetter::HOSTNAME,
    },
    {
        .name = "setPort",
        .value = UrlSetter::PORT,
    },
};

}

std::optional<UrlSetter> UrlSetterTable::find(llvm::StringRef name) {
  for (const UrlSetterInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
