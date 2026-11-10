#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker_errors.h"

namespace tz {

std::string ErrorTextStreamNew::abstract(llvm::StringRef name, bool readable, bool writable) {
  llvm::StringRef hook = readable           ? "onRead"
                         : writable         ? "onWrite"
                         : name == "Duplex" ? "onRead' and 'onWrite"
                                            : "onTransform";
  std::string example = readable   ? "class Lines extends Readable<string> { protected onRead() "
                                     "{ ... } }"
                        : writable ? "class Sink extends Writable<string> { protected "
                                     "onWrite(chunk, encoding, callback) { ... } }"
                                   : "class Upper extends " + name.str() + "<string> { ... }";

  return "'" + name.str() +
         "' is abstract: write a class that extends it and "
         "puts its work in '" +
         hook.str() + "', e.g. '" + example + "'" +
         (readable ? "; for a stream of values you already have, use "
                     "'Readable.from(values)'"
                   : "");
}

std::string ErrorTextStreamNew::untyped(llvm::StringRef name) {
  return "'" + name.str() + "' needs the type of its chunks, e.g. 'new " + name.str() +
         "<Buffer>()', 'new " + name.str() + "<string>()' or 'new " + name.str() + "<User>()'";
}

}
