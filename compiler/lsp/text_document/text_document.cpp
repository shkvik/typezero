#include "compiler/lsp/text_document/text_document.h"
#include "compiler/load/source_overlay/source_overlay.h"
#include <algorithm>
#include <utility>

namespace tz {

TextDocument::TextDocument(std::string uri, std::string path, std::string text)
    : uri_(std::move(uri)), path_(std::move(path)), key_(SourceOverlay::key(path_)),
      text_(std::move(text)) {}

size_t TextDocument::offset(const llvm::json::Object *position) const {
  if (!position) {
    return text_.size();
  }
  int64_t line = position->getInteger("line").value_or(0);
  int64_t character = position->getInteger("character").value_or(0);
  size_t at = 0;
  for (int64_t i = 0; i < line; ++i) {
    size_t next = text_.find('\n', at);
    if (next == std::string::npos) {
      return text_.size();
    }
    at = next + 1;
  }
  int64_t units = 0;
  while (at < text_.size() && text_[at] != '\n' && units < character) {
    unsigned char byte = static_cast<unsigned char>(text_[at]);
    size_t length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
    units += length == 4 ? 2 : 1;
    at += length;
  }

  return std::min(at, text_.size());
}

void TextDocument::apply(const llvm::json::Object &change) {
  std::optional<llvm::StringRef> text = change.getString("text");
  if (!text) {
    return;
  }
  const llvm::json::Object *range = change.getObject("range");
  if (!range) {
    text_ = text->str();
    return;
  }
  size_t start = offset(range->getObject("start"));
  size_t end = offset(range->getObject("end"));
  if (end < start) {
    std::swap(start, end);
  }
  text_.replace(start, end - start, text->str());
}

}
