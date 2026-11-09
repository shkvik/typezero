#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const BytesMethodInfo kBytesMethods[] = {
    {
        .name = "at",
        .method = BytesMethod::AT,
        .form = TypedArrayMethod{},
    },
    {
        .name = "copyWithin",
        .method = BytesMethod::COPY_WITHIN,
        .form = TypedArrayMethod{},
    },
    {
        .name = "entries",
        .method = BytesMethod::ENTRIES,
        .form = TypedArrayMethod{},
    },
    {
        .name = "every",
        .method = BytesMethod::EVERY,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "fill",
        .method = BytesMethod::FILL,
        .form = TypedArrayMethod{},
    },
    {
        .name = "filter",
        .method = BytesMethod::FILTER,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "find",
        .method = BytesMethod::FIND,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "findIndex",
        .method = BytesMethod::FIND_INDEX,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "findLast",
        .method = BytesMethod::FIND_LAST,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "findLastIndex",
        .method = BytesMethod::FIND_LAST_INDEX,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "forEach",
        .method = BytesMethod::FOR_EACH,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "includes",
        .method = BytesMethod::INCLUDES,
        .form = TypedArrayMethod{},
    },
    {
        .name = "indexOf",
        .method = BytesMethod::INDEX_OF,
        .form = TypedArrayMethod{},
    },
    {
        .name = "join",
        .method = BytesMethod::JOIN,
        .form = TypedArrayMethod{},
    },
    {
        .name = "keys",
        .method = BytesMethod::KEYS,
        .form = TypedArrayMethod{},
    },
    {
        .name = "lastIndexOf",
        .method = BytesMethod::LAST_INDEX_OF,
        .form = TypedArrayMethod{},
    },
    {
        .name = "map",
        .method = BytesMethod::MAP,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "reduce",
        .method = BytesMethod::REDUCE,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "reduceRight",
        .method = BytesMethod::REDUCE_RIGHT,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "reverse",
        .method = BytesMethod::REVERSE,
        .form = TypedArrayMethod{},
    },
    {
        .name = "set",
        .method = BytesMethod::SET,
        .form = TypedArrayMethod{},
    },
    {
        .name = "slice",
        .method = BytesMethod::SLICE,
        .form = TypedArrayMethod{},
    },
    {
        .name = "some",
        .method = BytesMethod::SOME,
        .form = TypedArrayCallbackMethod{},
    },
    {
        .name = "sort",
        .method = BytesMethod::SORT,
        .form = TypedArrayMethod{},
    },
    {
        .name = "subarray",
        .method = BytesMethod::SUBARRAY,
        .form = TypedArrayMethod{},
    },
    {
        .name = "toReversed",
        .method = BytesMethod::TO_REVERSED,
        .form = TypedArrayMethod{},
    },
    {
        .name = "toSorted",
        .method = BytesMethod::TO_SORTED,
        .form = TypedArrayMethod{},
    },
    {
        .name = "toString",
        .method = BytesMethod::TO_STRING,
        .form = TypedArrayMethod{},
    },
    {
        .name = "values",
        .method = BytesMethod::VALUES,
        .form = TypedArrayMethod{},
    },
    {
        .name = "with",
        .method = BytesMethod::WITH,
        .form = TypedArrayMethod{},
    },
    {
        .name = "flatMap",
        .method = BytesMethod::FLAT_MAP,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "toLocaleString",
        .method = BytesMethod::TO_LOCALE_STRING,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "toJSON",
        .method = BytesMethod::TO_JSON,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "equals",
        .method = BytesMethod::EQUALS,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "compare",
        .method = BytesMethod::COMPARE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "copy",
        .method = BytesMethod::COPY,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "write",
        .method = BytesMethod::WRITE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "swap16",
        .method = BytesMethod::SWAP16,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "swap32",
        .method = BytesMethod::SWAP32,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "swap64",
        .method = BytesMethod::SWAP64,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "resize",
        .method = BytesMethod::RESIZE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "transfer",
        .method = BytesMethod::TRANSFER,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "transferToFixedLength",
        .method = BytesMethod::TRANSFER_TO_FIXED_LENGTH,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "encode",
        .method = BytesMethod::ENCODE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "encodeInto",
        .method = BytesMethod::ENCODE_INTO,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "decode",
        .method = BytesMethod::DECODE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "isView",
        .method = BytesMethod::IS_VIEW,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "of",
        .method = BytesMethod::OF,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "from",
        .method = BytesMethod::FROM,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "parse",
        .method = BytesMethod::PARSE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "alloc",
        .method = BytesMethod::ALLOC,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "allocUnsafe",
        .method = BytesMethod::ALLOC_UNSAFE,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "allocUnsafeSlow",
        .method = BytesMethod::ALLOC_UNSAFE_SLOW,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "concat",
        .method = BytesMethod::CONCAT,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "byteLength",
        .method = BytesMethod::BYTE_LENGTH,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "isBuffer",
        .method = BytesMethod::IS_BUFFER,
        .form = NotOnTypedArrayMethod{},
    },
    {
        .name = "isEncoding",
        .method = BytesMethod::IS_ENCODING,
        .form = NotOnTypedArrayMethod{},
    },
};

}

const BytesMethodInfo *BytesMethodTable::find(llvm::StringRef name) {
  for (const BytesMethodInfo &method : kBytesMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

std::optional<BytesMethod> BytesMethodTable::findMethod(llvm::StringRef name) {
  const BytesMethodInfo *info = BytesMethodTable::find(name);
  if (info == nullptr) {
    return std::nullopt;
  }

  return info->method;
}

llvm::StringRef BytesMethodTable::getName(BytesMethod method) {
  for (const BytesMethodInfo &info : kBytesMethods) {
    if (info.method == method) {
      return info.name;
    }
  }
  llvm_unreachable("bytes method missing from the table");
}

}
