#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const GlobalNameInfo kGlobalNames[] = {
    {
        .name = "Array",
        .global = GlobalName::ARRAY,
    },
    {
        .name = "BigInt",
        .global = GlobalName::BIG_INT,
    },
    {
        .name = "BigInt64Array",
        .global = GlobalName::BIG_INT64_ARRAY,
    },
    {
        .name = "BigUint64Array",
        .global = GlobalName::BIG_UINT64_ARRAY,
    },
    {
        .name = "Bit",
        .global = GlobalName::BIT,
    },
    {
        .name = "Boolean",
        .global = GlobalName::BOOLEAN,
    },
    {
        .name = "Buffer",
        .global = GlobalName::BUFFER,
    },
    {
        .name = "clearImmediate",
        .global = GlobalName::CLEAR_IMMEDIATE,
    },
    {
        .name = "clearInterval",
        .global = GlobalName::CLEAR_INTERVAL,
    },
    {
        .name = "clearTimeout",
        .global = GlobalName::CLEAR_TIMEOUT,
    },
    {
        .name = "console",
        .global = GlobalName::CONSOLE,
    },
    {
        .name = "crypto",
        .global = GlobalName::CRYPTO,
    },
    {
        .name = "Date",
        .global = GlobalName::DATE,
    },
    {
        .name = "decimal",
        .global = GlobalName::DECIMAL,
    },
    {
        .name = "decodeURI",
        .global = GlobalName::DECODE_URI,
    },
    {
        .name = "decodeURIComponent",
        .global = GlobalName::DECODE_URI_COMPONENT,
    },
    {
        .name = "embedBytes",
        .global = GlobalName::EMBED_BYTES,
    },
    {
        .name = "embedText",
        .global = GlobalName::EMBED_TEXT,
    },
    {
        .name = "encodeURI",
        .global = GlobalName::ENCODE_URI,
    },
    {
        .name = "encodeURIComponent",
        .global = GlobalName::ENCODE_URI_COMPONENT,
    },
    {
        .name = "err",
        .global = GlobalName::ERR,
    },
    {
        .name = "Err",
        .global = GlobalName::ERR_TYPE,
    },
    {
        .name = "escape",
        .global = GlobalName::ESCAPE,
    },
    {
        .name = "globalThis",
        .global = GlobalName::GLOBAL_THIS,
    },
    {
        .name = "Intl",
        .global = GlobalName::INTL,
    },
    {
        .name = "Iterator",
        .global = GlobalName::ITERATOR,
    },
    {
        .name = "JSON",
        .global = GlobalName::JSON,
    },
    {
        .name = "Json",
        .global = GlobalName::JSON_TYPE,
    },
    {
        .name = "Map",
        .global = GlobalName::MAP,
    },
    {
        .name = "Math",
        .global = GlobalName::MATH,
    },
    {
        .name = "Number",
        .global = GlobalName::NUMBER,
    },
    {
        .name = "Object",
        .global = GlobalName::OBJECT,
    },
    {
        .name = "ok",
        .global = GlobalName::OK,
    },
    {
        .name = "Ok",
        .global = GlobalName::OK_TYPE,
    },
    {
        .name = "parseFloat",
        .global = GlobalName::PARSE_FLOAT,
    },
    {
        .name = "parseInt",
        .global = GlobalName::PARSE_INT,
    },
    {
        .name = "performance",
        .global = GlobalName::PERFORMANCE,
    },
    {
        .name = "process",
        .global = GlobalName::PROCESS,
    },
    {
        .name = "Promise",
        .global = GlobalName::PROMISE,
    },
    {
        .name = "queueMicrotask",
        .global = GlobalName::QUEUE_MICROTASK,
    },
    {
        .name = "Reflect",
        .global = GlobalName::REFLECT,
    },
    {
        .name = "RegExp",
        .global = GlobalName::REG_EXP,
    },
    {
        .name = "Result",
        .global = GlobalName::RESULT,
    },
    {
        .name = "Set",
        .global = GlobalName::SET,
    },
    {
        .name = "setImmediate",
        .global = GlobalName::SET_IMMEDIATE,
    },
    {
        .name = "setInterval",
        .global = GlobalName::SET_INTERVAL,
    },
    {
        .name = "setTimeout",
        .global = GlobalName::SET_TIMEOUT,
    },
    {
        .name = "String",
        .global = GlobalName::STRING,
    },
    {
        .name = "Symbol",
        .global = GlobalName::SYMBOL,
    },
    {
        .name = "unescape",
        .global = GlobalName::UNESCAPE,
    },
    {
        .name = "WeakMap",
        .global = GlobalName::WEAK_MAP,
    },
    {
        .name = "WeakRef",
        .global = GlobalName::WEAK_REF,
    },
    {
        .name = "WeakSet",
        .global = GlobalName::WEAK_SET,
    },
};

}

std::optional<GlobalName> GlobalNameTable::find(llvm::StringRef name) {
  for (const GlobalNameInfo &info : kGlobalNames) {
    if (info.name == name) {
      return info.global;
    }
  }

  return std::nullopt;
}

llvm::StringRef GlobalNameTable::getName(GlobalName global) {
  for (const GlobalNameInfo &info : kGlobalNames) {
    if (info.global == global) {
      return info.name;
    }
  }
  llvm_unreachable("global name missing from the table");
}

}
