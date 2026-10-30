#include "compiler/program/source_places/source_places.h"
#include "compiler/program/module/module.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

bool isWordByte(char c) {
  unsigned char byte = static_cast<unsigned char>(c);

  return byte >= 0x80 || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '$';
}

}

SourcePlaces::SourcePlaces(const llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

const char *SourcePlaces::tokenEnd(const char *at, const char *end) {
  if (at == end) {
    return at;
  }
  if (isWordByte(*at)) {
    while (at < end && isWordByte(*at)) {
      ++at;
    }
    return at;
  }
  if (*at == '"' || *at == '\'' || *at == '`') {
    char quote = *at++;
    while (at < end && *at != quote && *at != '\n') {
      if (*at == '\\' && at + 1 < end) {
        ++at;
      }
      ++at;
    }
    return at < end && *at == quote ? at + 1 : at;
  }
  if (*at == '\n' || *at == '\r') {
    return at;
  }

  return at + 1;
}

unsigned SourcePlaces::utf16Length(llvm::StringRef text) {
  unsigned units = 0;
  for (char c : text) {
    unsigned char byte = static_cast<unsigned char>(c);
    if ((byte & 0xC0) == 0x80) {
      continue;
    }
    units += byte >= 0xF0 ? 2 : 1;
  }

  return units;
}

std::string SourcePlaces::absolute(llvm::StringRef path) {
  llvm::SmallString<256> full(path);
  llvm::sys::fs::make_absolute(full);
  llvm::sys::path::remove_dots(full, true);
  llvm::sys::path::native(full);

  return std::string(full);
}

unsigned SourcePlaces::bufferOf(const char *at) const {
  if (!at) {
    return 0;
  }
  if (last_ != 0) {
    const llvm::MemoryBuffer *buffer = sourceMgr_.getMemoryBuffer(last_);
    if (at >= buffer->getBufferStart() && at <= buffer->getBufferEnd()) {
      return last_;
    }
  }
  unsigned id = sourceMgr_.FindBufferContainingLoc(llvm::SMLoc::getFromPointer(at));
  if (id != 0) {
    last_ = id;
  }

  return id;
}

void SourcePlaces::position(unsigned id, const char *at, unsigned &line,
                            unsigned &character) const {
  const llvm::MemoryBuffer *buffer = sourceMgr_.getMemoryBuffer(id);
  auto [row, column] = sourceMgr_.getLineAndColumn(llvm::SMLoc::getFromPointer(at), id);
  const char *start = at - (column - 1);
  llvm::StringRef before(start, column - 1);
  if (start == buffer->getBufferStart()) {
    before.consume_front("\xEF\xBB\xBF");
  }
  line = row - 1;
  character = utf16Length(before);
}

void SourcePlaces::nameFiles(const std::vector<Module *> &modules) {
  for (const Module *module : modules) {
    for (const auto &[id, file] : module->bufferFiles) {
      paths_[id] = {absolute(file), true};
    }
  }
}

std::string SourcePlaces::pathOf(unsigned id, bool &file) const {
  auto found = paths_.find(id);
  if (found == paths_.end()) {
    llvm::StringRef name = sourceMgr_.getMemoryBuffer(id)->getBufferIdentifier();
    bool regular = llvm::sys::fs::is_regular_file(name);
    found = paths_.try_emplace(id, regular ? absolute(name) : name.str(), regular).first;
  }
  file = found->second.second;

  return found->second.first;
}

std::optional<SourcePlace> SourcePlaces::locate(llvm::SMLoc loc) const {
  unsigned id = bufferOf(loc.getPointer());
  if (id == 0) {
    return std::nullopt;
  }
  const char *end = tokenEnd(loc.getPointer(), sourceMgr_.getMemoryBuffer(id)->getBufferEnd());

  SourcePlace place;
  place.path = pathOf(id, place.file);
  position(id, loc.getPointer(), place.line, place.character);
  place.endLine = place.line;
  place.endCharacter =
      place.character + utf16Length(llvm::StringRef(loc.getPointer(), end - loc.getPointer()));

  return place;
}

std::optional<SourcePlace> SourcePlaces::find(llvm::SMLoc loc) const {
  std::optional<SourcePlace> place = locate(loc);
  if (!place || !place->file) {
    return std::nullopt;
  }

  return place;
}

std::optional<SourcePlace> SourcePlaces::span(const char *begin, const char *end) const {
  unsigned id = bufferOf(begin);
  if (id == 0 || end < begin) {
    return std::nullopt;
  }

  SourcePlace place;
  place.path = pathOf(id, place.file);
  position(id, begin, place.line, place.character);
  position(id, end, place.endLine, place.endCharacter);

  return place;
}

llvm::StringRef SourcePlaces::lineText(llvm::SMLoc loc) const {
  unsigned id = bufferOf(loc.getPointer());
  if (id == 0) {
    return "";
  }
  llvm::StringRef text = sourceMgr_.getMemoryBuffer(id)->getBuffer();
  size_t at = static_cast<size_t>(loc.getPointer() - text.data());
  size_t start = text.rfind('\n', at);
  start = start == llvm::StringRef::npos ? 0 : start + 1;
  size_t end = text.find('\n', at);

  return text.slice(start, end).rtrim("\r");
}

std::string SourcePlaces::describe(llvm::SMLoc loc) const {
  unsigned id = bufferOf(loc.getPointer());
  if (id == 0) {
    return "";
  }
  auto [line, column] = sourceMgr_.getLineAndColumn(loc, id);
  llvm::StringRef name = sourceMgr_.getMemoryBuffer(id)->getBufferIdentifier();

  return name.str() + ":" + std::to_string(line) + ":" + std::to_string(column);
}

}
