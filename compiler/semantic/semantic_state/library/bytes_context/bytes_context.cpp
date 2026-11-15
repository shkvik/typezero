#include "compiler/semantic/semantic_state/library/bytes_context/bytes_context.h"

namespace tz {

void BytesContext::enterDecoderParse() {
  this->decoderParseDepth += 1;
}

void BytesContext::leaveDecoderParse() {
  this->decoderParseDepth -= 1;
}

bool BytesContext::isInsideDecoderParse() const {
  return this->decoderParseDepth > 0;
}

}
