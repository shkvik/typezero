#include "compiler/program/library/crypto/crypto_stream_method_table/crypto_stream_method_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kStreamMethods[] = {
    "write",   "end",         "pipe", "unpipe", "read",           "on",
    "once",    "off",         "emit", "pause",  "resume",         "setEncoding",
    "destroy", "addListener", "cork", "uncork", "removeListener", "setDefaultEncoding"};

}

bool CryptoStreamMethodTable::isStreamMethod(llvm::StringRef name) {
  return llvm::is_contained(kStreamMethods, name);
}

}
