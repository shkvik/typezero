#include "compiler/semantic/library_v2/bytes/library_bytes.h"

namespace tz {

namespace {

const Type::Bytes kNamedKinds[] = {
    Type::Bytes::Uint8,        Type::Bytes::Int8,
    Type::Bytes::Uint8Clamped, Type::Bytes::Int16,
    Type::Bytes::Uint16,       Type::Bytes::Int32,
    Type::Bytes::Uint32,       Type::Bytes::Float16,
    Type::Bytes::Float32,      Type::Bytes::Float64,
    Type::Bytes::Buffer,       Type::Bytes::ArrayBuffer,
    Type::Bytes::DataView,     Type::Bytes::TextEncoder,
    Type::Bytes::TextDecoder,  Type::Bytes::FatalTextDecoder,
};

}

const Type *LibraryBytes::bytesTypeNamed(llvm::StringRef name) const {
  for (Type::Bytes kind : kNamedKinds) {
    const Type *type = Type::bytesType(kind);
    if (type->name() == name) {
      return type;
    }
  }

  return nullptr;
}

}
