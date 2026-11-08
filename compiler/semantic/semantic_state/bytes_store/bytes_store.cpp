#include "compiler/semantic/semantic_state/bytes_store/bytes_store.h"

namespace tz {

void BytesStore::enterDecoderParse() {
  this->decoderParseDepth += 1;
}

void BytesStore::leaveDecoderParse() {
  this->decoderParseDepth -= 1;
}

bool BytesStore::isInsideDecoderParse() const {
  return this->decoderParseDepth > 0;
}

}
