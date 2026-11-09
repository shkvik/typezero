#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const StringMethodInfo kStringMethods[] = {
    {
        .name = "charCodeAt",
        .method = StringMethod::CHAR_CODE_AT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrCharCodeAt,
                        .minArgs = 1,
                        .maxArgs = 1,
                        .args = "p",
                        .result = StringResult::INT,
                    },
            },
    },
    {
        .name = "charAt",
        .method = StringMethod::CHAR_AT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrCharAt,
                        .minArgs = 0,
                        .maxArgs = 1,
                        .args = "p",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "codePointAt",
        .method = StringMethod::CODE_POINT_AT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrCodePointAt,
                        .minArgs = 1,
                        .maxArgs = 1,
                        .args = "p",
                        .result = StringResult::INT,
                    },
            },
    },
    {
        .name = "at",
        .method = StringMethod::AT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrAt,
                        .minArgs = 1,
                        .maxArgs = 1,
                        .args = "p",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "slice",
        .method = StringMethod::SLICE,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrSlice,
                        .minArgs = 0,
                        .maxArgs = 2,
                        .args = "p",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "substring",
        .method = StringMethod::SUBSTRING,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrSubstring,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "p",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "indexOf",
        .method = StringMethod::INDEX_OF,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrIndexOf,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::INT,
                    },
            },
    },
    {
        .name = "lastIndexOf",
        .method = StringMethod::LAST_INDEX_OF,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrLastIndexOf,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::INT,
                    },
            },
    },
    {
        .name = "includes",
        .method = StringMethod::INCLUDES,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrIncludes,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::BOOL,
                    },
            },
    },
    {
        .name = "startsWith",
        .method = StringMethod::STARTS_WITH,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrStartsWith,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::BOOL,
                    },
            },
    },
    {
        .name = "endsWith",
        .method = StringMethod::ENDS_WITH,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrEndsWith,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::BOOL,
                    },
            },
    },
    {
        .name = "repeat",
        .method = StringMethod::REPEAT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrRepeat,
                        .minArgs = 1,
                        .maxArgs = 1,
                        .args = "p",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "toString",
        .method = StringMethod::TO_STRING,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrToString,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "concat",
        .method = StringMethod::CONCAT,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrConcat,
                        .minArgs = 0,
                        .maxArgs = StringMethodTable::kAnyCount,
                        .args = "*",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "split",
        .method = StringMethod::SPLIT,
        .form =
            TextOrPatternStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrSplit,
                        .minArgs = 0,
                        .maxArgs = 2,
                        .args = "sp",
                        .result = StringResult::STRINGS,
                    },
            },
    },
    {
        .name = "trim",
        .method = StringMethod::TRIM,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrTrim,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "trimStart",
        .method = StringMethod::TRIM_START,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrTrimStart,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "trimEnd",
        .method = StringMethod::TRIM_END,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrTrimEnd,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "padStart",
        .method = StringMethod::PAD_START,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrPadStart,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "ps",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "padEnd",
        .method = StringMethod::PAD_END,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrPadEnd,
                        .minArgs = 1,
                        .maxArgs = 2,
                        .args = "ps",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "toUpperCase",
        .method = StringMethod::TO_UPPER_CASE,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrToUpperCase,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "toLowerCase",
        .method = StringMethod::TO_LOWER_CASE,
        .form =
            TextStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrToLowerCase,
                        .minArgs = 0,
                        .maxArgs = 0,
                        .args = "",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "replace",
        .method = StringMethod::REPLACE,
        .form =
            TextOrPatternStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrReplace,
                        .minArgs = 2,
                        .maxArgs = 2,
                        .args = "ss",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "replaceAll",
        .method = StringMethod::REPLACE_ALL,
        .form =
            TextOrPatternStringMethod{
                .call =
                    StringCall{
                        .builtin = Builtin::StrReplaceAll,
                        .minArgs = 2,
                        .maxArgs = 2,
                        .args = "ss",
                        .result = StringResult::STRING,
                    },
            },
    },
    {
        .name = "match",
        .method = StringMethod::MATCH,
        .form = PatternStringMethod{},
    },
    {
        .name = "matchAll",
        .method = StringMethod::MATCH_ALL,
        .form = PatternStringMethod{},
    },
    {
        .name = "search",
        .method = StringMethod::SEARCH,
        .form = PatternStringMethod{},
    },
    {
        .name = "localeCompare",
        .method = StringMethod::LOCALE_COMPARE,
        .form = LocaleStringMethod{},
    },
    {
        .name = "normalize",
        .method = StringMethod::NORMALIZE,
        .form = LocaleStringMethod{},
    },
};

}

const StringMethodInfo *StringMethodTable::find(llvm::StringRef name) {
  for (const StringMethodInfo &method : kStringMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

const StringCall *StringMethodTable::findCall(const StringMethodInfo &method) {
  switch (method.form.kind()) {
    case StringMethodKind::TEXT:
      return &method.form.as<TextStringMethod>()->call;
    case StringMethodKind::TEXT_OR_PATTERN:
      return &method.form.as<TextOrPatternStringMethod>()->call;
    case StringMethodKind::PATTERN:
    case StringMethodKind::LOCALE:
      return nullptr;
  }
  llvm_unreachable("unknown string method kind");
}

llvm::ArrayRef<StringMethodInfo> StringMethodTable::getMethods() {
  return kStringMethods;
}

}
