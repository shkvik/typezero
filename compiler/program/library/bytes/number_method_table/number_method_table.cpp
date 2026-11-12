#include "compiler/program/library/bytes/number_method_table/number_method_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kGetPrefix = "get";
constexpr llvm::StringLiteral kSetPrefix = "set";
constexpr llvm::StringLiteral kReadPrefix = "read";
constexpr llvm::StringLiteral kWritePrefix = "write";
constexpr llvm::StringLiteral kLittleEndianSuffix = "LE";
constexpr llvm::StringLiteral kBigEndianSuffix = "BE";
constexpr llvm::StringLiteral kSingleByteSuffix = "8";
constexpr llvm::StringLiteral kFloatPrefix = "Float";
constexpr llvm::StringLiteral kBufferOnlyPrefix = "UInt";
constexpr llvm::StringLiteral kSignedVariable = "Int";

const NumberKindInfo kKinds[] = {
    {.name = "Int8", .kind = Type::Bytes::Int8},
    {.name = "Uint8", .kind = Type::Bytes::Uint8},
    {.name = "UInt8", .kind = Type::Bytes::Uint8},
    {.name = "Int16", .kind = Type::Bytes::Int16},
    {.name = "Uint16", .kind = Type::Bytes::Uint16},
    {.name = "UInt16", .kind = Type::Bytes::Uint16},
    {.name = "Int32", .kind = Type::Bytes::Int32},
    {.name = "Uint32", .kind = Type::Bytes::Uint32},
    {.name = "UInt32", .kind = Type::Bytes::Uint32},
    {.name = "Float16", .kind = Type::Bytes::Float16},
    {.name = "Float32", .kind = Type::Bytes::Float32},
    {.name = "Float64", .kind = Type::Bytes::Float64},
    {.name = "Float", .kind = Type::Bytes::Float32},
    {.name = "Double", .kind = Type::Bytes::Float64},
};

const llvm::StringLiteral kBufferFloatNames[] = {"Float", "Double"};

const llvm::StringLiteral kBigIntPrefixes[] = {"BigInt64", "BigUint64", "BigUInt64"};

const llvm::StringLiteral kVariableNames[] = {"Int", "UInt", "Uint"};

}

std::optional<BytesNumberMethod> NumberMethodTable::parse(llvm::StringRef name, bool dataView) {
  auto method = BytesNumberMethod();
  llvm::StringRef rest = name;
  if (!NumberMethodTable::readAccess(rest, dataView, method)) {
    return std::nullopt;
  }
  if (!NumberMethodTable::readKind(rest, dataView, method)) {
    return std::nullopt;
  }

  return method;
}

bool NumberMethodTable::readAccess(llvm::StringRef &rest, bool dataView,
                                   BytesNumberMethod &method) {
  if (dataView) {
    return rest.consume_front(kGetPrefix) || (method.write = rest.consume_front(kSetPrefix));
  }
  if (!rest.consume_front(kReadPrefix) && !(method.write = rest.consume_front(kWritePrefix))) {
    return false;
  }
  if (rest.consume_back(kLittleEndianSuffix)) {
    method.big = false;
    return true;
  }
  if (rest.consume_back(kBigEndianSuffix)) {
    method.big = true;
    return true;
  }

  return rest.ends_with(kSingleByteSuffix);
}

bool NumberMethodTable::readKind(llvm::StringRef rest, bool dataView, BytesNumberMethod &method) {
  for (const NumberKindInfo &info : kKinds) {
    if (rest != info.name) {
      continue;
    }
    llvm::StringRef kindName = info.name;
    bool isFloatName = llvm::is_contained(kBufferFloatNames, kindName);
    bool isDataViewName = kindName.starts_with(kFloatPrefix) && !isFloatName;
    bool isBufferOnly = isFloatName || kindName.starts_with(kBufferOnlyPrefix);
    if ((dataView && isBufferOnly) || (!dataView && isDataViewName)) {
      return false;
    }
    method.kind = info.kind;
    return true;
  }
  for (llvm::StringRef prefix : kBigIntPrefixes) {
    if (rest.starts_with(prefix)) {
      method.bigint = true;
      return true;
    }
  }
  if (!dataView && llvm::is_contained(kVariableNames, rest)) {
    method.variable = true;
    method.isSigned = rest == kSignedVariable;
    return true;
  }

  return false;
}

}
