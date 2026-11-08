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

std::string ErrorTextStreamNew::untyped(llvm::StringRef what, llvm::StringRef name, bool pushes) {
  return "cannot infer the type of the " + what.str() + " of this object-mode " + name.str() +
         ": no code of the program " + (pushes ? "pushes into it" : "writes to it") +
         "; give the type, e.g. 'new " + name.str() + "<User>({ " + "objectMode: true, ... })'";
}

std::string ErrorTextStreamSide::written() {
  return "written chunks";
}

std::string ErrorTextStreamSide::given() {
  return "chunks it gives";
}

}
