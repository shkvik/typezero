#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker_errors.h"

namespace tz {

std::string ErrorTextBytesIndex::notIndexed(const Type *type) {
  return "type '" + type->name().str() + "' cannot be indexed; " +
         (type->bytes() == Type::Bytes::DataView ? "read its bytes with getUint8(i) and the like"
          : type->bytes() == Type::Bytes::ArrayBuffer ? "read its bytes through a view, e.g. 'new "
                                                        "Uint8Array(buffer)[i]'"
                                                      : "it holds no elements");
}

std::string ErrorTextBytesIndex::store(llvm::StringRef target, llvm::StringRef object,
                                       llvm::StringRef position, std::optional<int64_t> at) {
  std::string check =
      at ? object.str() + ".length > " + std::to_string(*at)
         : position.str() + " >= 0 && " + position.str() + " < " + object.str() + ".length";

  return "cannot prove that the index is within the " + target.str() +
         " (0 <= index < length): it never grows, and JavaScript would "
         "drop the write; check the index first, e.g. 'if (" +
         check + ")'";
}

std::string ErrorTextBytesMember::deprecated(llvm::StringRef name) {
  return "'" + name.str() +
         "' of a Buffer is deprecated in node and not "
         "supported; use '" +
         (name == "parent" ? "buffer" : "byteOffset") + "'";
}

std::string ErrorTextBytesMember::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextBytesMember::notCalled(llvm::StringRef name, llvm::StringRef type) {
  return ErrorTextBytesMember::noMember(name, type) + "; methods must be called";
}

std::string ErrorTextBytesMember::narrow(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' of '" + type.str() +
         "' is not supported yet; narrow it first";
}

std::string ErrorTextBytesMember::lacking(llvm::StringRef name, llvm::StringRef lacking,
                                          llvm::StringRef type, llvm::StringRef with) {
  std::string message = "property '" + name.str() + "' does not exist on '" + lacking.str() +
                        "', which '" + type.str() + "' may be; narrow it first";
  if (!with.empty()) {
    message += ", e.g. 'if (x instanceof " + with.str() + ")'";
  }

  return message;
}

std::string ErrorTextBytesMember::callOnly(llvm::StringRef object, llvm::StringRef name) {
  return "'" + object.str() + "." + name.str() + "' can be used only in a call, e.g. '" +
         object.str() + "." + name.str() + "(...)'";
}

std::string ErrorTextBytesString::noText(llvm::StringRef type, bool buffer) {
  return "a value of type '" + type.str() +
         "' cannot be converted to a string: JavaScript would give '[object " + type.str() +
         "]'; print it with console.log" +
         (buffer ? ", or the text of its bytes with 'new TextDecoder().decode(buffer)'" : "");
}

std::string ErrorTextBufferEncoding::unknown(llvm::StringRef name) {
  return "unknown encoding '" + name.str() +
         "' (node throws a TypeError); Buffer knows utf8, utf16le, "
         "latin1, ascii, base64, base64url and hex";
}

std::string ErrorTextBufferEncoding::notKnown() {
  return "an encoding known only when the program runs may be one Buffer does "
         "not know, which node would throw a TypeError for; check it first, "
         "e.g. 'if (enc == \"hex\" || enc == \"base64\")', or give it the type "
         "'BufferEncoding'";
}

std::string ErrorTextArrayBuffer::notYet(llvm::StringRef name) {
  return "'" + name.str() +
         "' is not supported yet: an ArrayBuffer keeps its "
         "size and its memory (docs/bytes.md, section 7)";
}

std::string ErrorTextArrayBuffer::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on type 'ArrayBuffer'";
}

}
