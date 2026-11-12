#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"

namespace tz {

namespace {

const PreludeTypeInfo kRows[] = {
    {
        .name = "BufferEncoding",
        .type = PreludeType::BUFFER_ENCODING,
    },
    {
        .name = "DateError",
        .type = PreludeType::DATE_ERROR,
    },
    {
        .name = "DecimalError",
        .type = PreludeType::DECIMAL_ERROR,
    },
    {
        .name = "DecodeError",
        .type = PreludeType::DECODE_ERROR,
    },
    {
        .name = "EncodingError",
        .type = PreludeType::ENCODING_ERROR,
    },
    {
        .name = "JsonError",
        .type = PreludeType::JSON_ERROR,
    },
    {
        .name = "NumberError",
        .type = PreludeType::NUMBER_ERROR,
    },
    {
        .name = "QuestionError",
        .type = PreludeType::QUESTION_ERROR,
    },
    {
        .name = "ReflectError",
        .type = PreludeType::REFLECT_ERROR,
    },
    {
        .name = "ReflectFillError",
        .type = PreludeType::REFLECT_FILL_ERROR,
    },
    {
        .name = "ReflectFillValueError",
        .type = PreludeType::REFLECT_FILL_VALUE_ERROR,
    },
    {
        .name = "RegexError",
        .type = PreludeType::REGEX_ERROR,
    },
    {
        .name = "StreamError",
        .type = PreludeType::STREAM_ERROR,
    },
    {
        .name = "UriError",
        .type = PreludeType::URI_ERROR,
    },
};

}

llvm::StringRef PreludeTypeTable::getName(PreludeType type) {
  for (const PreludeTypeInfo &info : kRows) {
    if (info.type == type) {
      return info.name;
    }
  }

  return "";
}

}
