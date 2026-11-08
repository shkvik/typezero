#include "compiler/semantic/library/stream/stream_sources/stream_sources_errors.h"

namespace tz {

std::string ErrorTextStreamFrom::source(llvm::StringRef type) {
  return "'Readable.from' takes an iterable, an async iterable, a string "
         "or a Buffer, but this is '" +
         type.str() + "'";
}

}
