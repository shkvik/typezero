#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const KeywordInfo kRows[] = {
    {
        .name = "this",
        .value = Keyword::THIS,
    },
    {
        .name = "super",
        .value = Keyword::SUPER,
    },
    {
        .name = "arguments",
        .value = Keyword::ARGUMENTS,
    },
    {
        .name = "null",
        .value = Keyword::NULL_KEYWORD,
    },
    {
        .name = "undefined",
        .value = Keyword::UNDEFINED_KEYWORD,
    },
    {
        .name = "true",
        .value = Keyword::TRUE_KEYWORD,
    },
    {
        .name = "false",
        .value = Keyword::FALSE_KEYWORD,
    },
    {
        .name = "continue",
        .value = Keyword::CONTINUE_KEYWORD,
    },
};

}

std::optional<Keyword> KeywordTable::find(llvm::StringRef name) {
  for (const KeywordInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef KeywordTable::getName(Keyword value) {
  for (const KeywordInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("keyword missing from the table");
}

}
