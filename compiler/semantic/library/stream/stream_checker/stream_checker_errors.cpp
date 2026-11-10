#include "compiler/semantic/library/stream/stream_checker/stream_checker_errors.h"

namespace tz {

std::string ErrorTextStreamCall::overrides() {
  return "'overrides' takes an object of a class and the name of a "
         "method, as a string literal";
}

std::string ErrorTextStreamCall::noMethod(llvm::StringRef cls, llvm::StringRef method) {
  return "class '" + cls.str() + "' has no method '" + method.str() + "'";
}

std::string ErrorTextStreamCall::undefined(llvm::StringRef method) {
  return "'" + method.str() +
         "(undefined)' does not end a stream: a chunk is "
         "never undefined; end the stream with 'pushEnd()'";
}

std::string ErrorTextStreamCall::chunk(llvm::StringRef method, llvm::StringRef chunk,
                                       bool onlyStrings, llvm::StringRef type) {
  std::string takes = onlyStrings ? "a string" : "a string, a Buffer or a Uint8Array";

  return "'" + method.str() + "' of a stream of " + chunk.str() + " takes " + takes +
         ", but this is '" + type.str() + "'" +
         (onlyStrings ? "; a Writable<string> gives its onWrite() the "
                        "strings written; make it a Writable<Buffer> to "
                        "write bytes"
                      : "; to write objects, make the stream one of their type, e.g. "
                        "'class Users extends Readable<User>'");
}

std::string ErrorTextStreamCall::pipe() {
  return "'pipe' is not supported: it does not pass an error on, so the source stops and the "
         "destination stays open; join the streams with 'await pipeline(source, destination)', "
         "which gives a Result";
}

std::string ErrorTextStreamCall::noSide(llvm::StringRef type, llvm::StringRef name, bool reading) {
  return "'" + type.str() + "' has no " + name.str() +
         "(): " + (reading ? "it is not readable" : "it is not writable");
}

std::string ErrorTextStreamCall::endWithFn() {
  return "end() takes no callback: it gives a Promise of a Result, which says when the "
         "stream finished and whether it failed; 'const r = await w.end()'";
}

std::string ErrorTextStreamCall::endArgs() {
  return "end() takes a chunk and its encoding";
}

}
