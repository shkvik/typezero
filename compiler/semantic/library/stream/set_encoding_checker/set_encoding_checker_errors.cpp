#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker_errors.h"

namespace tz {

std::string ErrorTextSetEncoding::unreadable(llvm::StringRef type) {
  return "'" + type.str() + "' has no setEncoding(): it is not readable";
}

std::string ErrorTextSetEncoding::objectMode(llvm::StringRef chunk) {
  return "setEncoding() decodes the Buffers of a stream of bytes, and this is an "
         "object-mode stream of " +
         chunk.str() + "; push strings into it instead";
}

std::string ErrorTextSetEncoding::stdinPlace() {
  return "'process.stdin.setEncoding()' changes the type of the chunks of stdin for "
         "the code after it, so it goes in the top-level code of the program's "
         "main file, before any other use of process.stdin";
}

std::string ErrorTextSetEncoding::changes(llvm::StringRef why) {
  return "setEncoding() makes the chunks of the stream strings from here on, so its "
         "type changes here; " +
         why.str() +
         ": give the stream its encoding where it is "
         "created, e.g. 'createReadStream(path, \"utf8\")' or 'new Readable({ "
         "encoding: \"utf8\", ... })'";
}

std::string ErrorTextSetEncoding::usedBefore(llvm::StringRef how) {
  return "setEncoding() on a stream that is used elsewhere already, which sees its "
         "chunks as Buffers, but strings would come: " +
         how.str() +
         " before this; give the stream its encoding where it is created, e.g. "
         "'createReadStream(path, \"utf8\")', or call setEncoding() first";
}

std::string ErrorTextSetEncoding::usedHere() {
  return "the stream is used here";
}

std::string ErrorTextEncodingWhy::noConstant() {
  return "but this stream is not known by a constant";
}

std::string ErrorTextEncodingWhy::notConst(llvm::StringRef name) {
  return "but '" + name.str() + "' may hold another stream later; declare it with 'const'";
}

std::string ErrorTextEncodingWhy::fieldSet(llvm::StringRef field) {
  return "but '" + field.str() + "' is a field that code may set to another stream";
}

std::string ErrorTextEncodingWhy::ownBlock(llvm::StringRef name) {
  return "but this setEncoding() is in a block of its own (an if, a loop, a function) "
         "and '" +
         name.str() +
         "' is declared outside it, so the type would "
         "depend on whether it runs";
}

std::string ErrorTextEncodingUse::listeners(llvm::StringRef method) {
  return "it got listeners (" + method.str() + "())";
}

std::string ErrorTextEncodingUse::function() {
  return "a function written before uses it";
}

std::string ErrorTextEncodingUse::elsewhere() {
  return "it went elsewhere (passed, stored or returned)";
}

}
