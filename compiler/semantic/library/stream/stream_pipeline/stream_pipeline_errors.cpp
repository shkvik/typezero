#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline_errors.h"

namespace tz {

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

}
