#include "compiler/semantic/library/stream/library_stream_registry_errors.h"
#include "compiler/program/types/types.h"

namespace tz {

std::string ErrorTextStreamHook::params(const FuncSymbol *hook) {
  auto params = std::string();
  for (size_t i = 0; i < hook->params.size(); ++i) {
    if (i) {
      params += ", ";
    }
    llvm::StringRef name = hook->decl != nullptr && i < hook->decl->params.size()
                               ? hook->decl->params[i].name
                               : llvm::StringRef("arg");
    params += name.str() + ": " + hook->params[i]->name().str();
  }

  return params;
}

std::string ErrorTextStreamHook::other(llvm::StringRef name, llvm::StringRef params, bool chunks) {
  auto tail = std::string();
  if (chunks) {
    tail = "; for chunks of another type, give the class it extends type arguments, e.g. "
           "'extends Writable<string>'";
  }

  return "'" + name.str() + "' is what node:stream calls, as '" + name.str() + "(" + params.str() +
         ")', and a method with other parameters is another "
         "variant of the name, which it never calls; take those parameters, "
         "or leave out their types" +
         tail;
}

std::string ErrorTextSetEncoding::unreadable(llvm::StringRef type) {
  return "'" + type.str() + "' has no setEncoding(): it is not readable";
}

std::string ErrorTextSetEncoding::objectMode(llvm::StringRef chunk) {
  return "setEncoding() decodes the Buffers of a stream of bytes, and this is a stream of " +
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

std::string ErrorTextPipeline::notWritten(llvm::StringRef type) {
  return "pipeline() writes the chunks before it to each stream after the first, "
         "but '" +
         type.str() + "' is not writable";
}

std::string ErrorTextPipeline::chunkKind(llvm::StringRef fn, llvm::StringRef chunk,
                                         llvm::StringRef type, llvm::StringRef takes) {
  return fn.str() + "() writes chunks of " + chunk.str() + " to '" + type.str() +
         "', which takes " + takes.str();
}

std::string ErrorTextPipeline::notRead(llvm::StringRef type) {
  return "pipeline() reads each stream but the last, and '" + type.str() + "' is not readable";
}

std::string ErrorTextPipeline::source() {
  return "the first function of pipeline() is the source of the chunks: an async "
         "generator, e.g. 'async function* () { yield \"a\" }'";
}

std::string ErrorTextPipeline::function(bool last) {
  return last ? "the last function of pipeline() is an async generator over the "
                "chunks before it, or an async function that takes them and gives a "
                "Promise"
              : "a function in pipeline() is an async generator over the chunks "
                "before it, e.g. 'async function* (source) { for await (const chunk "
                "of source) yield chunk }'";
}

std::string ErrorTextPipeline::stage(llvm::StringRef type) {
  return "pipeline() takes streams, async generator functions over the chunks and, "
         "first, an iterable; this is '" +
         type.str() + "'";
}

std::string ErrorTextPipeline::callback() {
  return "pipeline() takes two streams or more and a callback, e.g. "
         "'pipeline(source, destination, (err) => ...)'";
}

std::string ErrorTextPipeline::streams() {
  return "pipeline() takes two streams or more, e.g. 'pipeline(source, destination)'";
}

std::string ErrorTextCompose::streams() {
  return "compose() takes the streams to make one Duplex of, e.g. 'compose(a, b)'";
}

std::string ErrorTextCompose::notStream(llvm::StringRef type) {
  return "compose() takes streams, and this is '" + type.str() +
         "'; to put a function between them, use pipeline()";
}

std::string ErrorTextStreamFrom::source(llvm::StringRef type) {
  return "'Readable.from' takes an iterable, an async iterable, a string "
         "or a Buffer, but this is '" +
         type.str() + "'";
}

}
