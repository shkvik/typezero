#include "compiler/knowledge/knowledge/knowledge.h"
#include "compiler/load/source_overlay/source_overlay.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <set>

namespace tz {

namespace {

bool covers(const SourcePlace &place, unsigned line, unsigned character) {
  if (line < place.line || line > place.endLine) {
    return false;
  }
  if (line == place.line && character < place.character) {
    return false;
  }

  return !(line == place.endLine && character > place.endCharacter);
}

unsigned size(const SourcePlace &place) {
  return (place.endLine - place.line) * 1000 + place.endCharacter;
}

}

std::string Knowledge::qualified(size_t symbol) const {
  const KnownSymbol &known = symbols[symbol];

  return known.container.empty() ? known.name : known.container + "." + known.name;
}

std::string Knowledge::sourceOf(size_t symbol) const {
  const KnownSymbol &known = symbols[symbol];
  if (!known.source.empty()) {
    return known.source;
  }
  const KnownModule &module = modules[known.module];
  auto buffer = llvm::MemoryBuffer::getFile(module.file.empty() ? known.place.path : module.file);
  if (!buffer) {
    return "";
  }
  llvm::StringRef text = (*buffer)->getBuffer();
  size_t start = 0;
  for (unsigned i = 0; i < known.extent.line && start != llvm::StringRef::npos; ++i) {
    start = text.find('\n', start);
    start = start == llvm::StringRef::npos ? start : start + 1;
  }
  if (start == llvm::StringRef::npos) {
    return "";
  }
  size_t end = start;
  for (unsigned i = known.extent.line; i <= known.extent.endLine && end != llvm::StringRef::npos;
       ++i) {
    end = text.find('\n', end);
    end = end == llvm::StringRef::npos ? end : end + 1;
  }

  return text.slice(start, end == llvm::StringRef::npos ? text.size() : end).rtrim("\r\n").str();
}

std::string Knowledge::relative(llvm::StringRef path) const {
  llvm::StringRef rest = path;
  if (!root.empty() && rest.size() > root.size() && rest.starts_with_insensitive(root) &&
      llvm::sys::path::is_separator(rest[root.size()])) {
    rest = rest.drop_front(root.size() + 1);
  }

  return llvm::sys::path::convert_to_slash(rest);
}

std::string Knowledge::where(const SourcePlace &place) const {
  return relative(place.path) + ":" + std::to_string(place.line + 1) + ":" +
         std::to_string(place.character + 1);
}

void Knowledge::index() const {
  if (indexed_ == symbols.size() + references.size() + modules.size() + 1) {
    return;
  }
  to_.assign(symbols.size(), {});
  from_.assign(symbols.size(), {});
  children_.assign(symbols.size(), {});
  byModule_.assign(modules.size(), {});
  importers_.assign(modules.size(), {});
  counts_.assign(symbols.size(), 0);
  for (size_t i = 0; i < references.size(); ++i) {
    to_[references[i].symbol].push_back(i);
    ++counts_[references[i].symbol];
    if (references[i].from != kNone) {
      from_[references[i].from].push_back(i);
    }
  }
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (symbols[i].parent != kNone) {
      children_[symbols[i].parent].push_back(i);
    }
    if (symbols[i].module < modules.size()) {
      byModule_[symbols[i].module].push_back(i);
    }
  }
  for (size_t m = 0; m < modules.size(); ++m) {
    for (size_t imported : modules[m].imports) {
      if (imported < modules.size()) {
        importers_[imported].push_back(m);
      }
    }
  }
  indexed_ = symbols.size() + references.size() + modules.size() + 1;
}

std::vector<size_t> Knowledge::referencesTo(size_t symbol) const {
  index();

  return to_[symbol];
}

std::vector<size_t> Knowledge::referencesFrom(size_t symbol) const {
  index();

  return from_[symbol];
}

const std::vector<size_t> &Knowledge::symbolsOf(size_t module) const {
  index();

  return byModule_[module];
}

std::vector<size_t> Knowledge::usersOf(size_t symbol) const {
  std::vector<size_t> found;
  std::set<size_t> seen;
  for (size_t reference : referencesTo(symbol)) {
    size_t from = references[reference].from;
    if (from != symbol && seen.insert(from).second) {
      found.push_back(from);
    }
  }

  return found;
}

std::vector<size_t> Knowledge::usesOf(size_t symbol) const {
  std::vector<size_t> found;
  std::set<size_t> seen;
  for (size_t reference : referencesFrom(symbol)) {
    size_t used = references[reference].symbol;
    if (used != symbol && seen.insert(used).second) {
      found.push_back(used);
    }
  }

  return found;
}

std::vector<size_t> Knowledge::members(size_t symbol) const {
  index();

  return children_[symbol];
}

size_t Knowledge::useCount(size_t symbol) const {
  index();

  return counts_[symbol];
}

std::vector<size_t> Knowledge::importersOf(size_t module) const {
  index();

  return importers_[module];
}

std::vector<size_t> Knowledge::named(llvm::StringRef name) const {
  std::vector<size_t> found;
  bool dotted = name.contains('.');
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (dotted ? qualified(i) == name : symbols[i].name == name) {
      found.push_back(i);
    }
  }
  std::stable_sort(found.begin(), found.end(),
                   [&](size_t a, size_t b) { return !library(a) && library(b); });

  return found;
}

std::optional<size_t> Knowledge::moduleAt(llvm::StringRef path) const {
  if (moduleKeys_.size() != modules.size()) {
    moduleKeys_.clear();
    for (const KnownModule &module : modules) {
      moduleKeys_.push_back(module.library ? std::string() : SourceOverlay::key(module.path));
    }
  }
  std::string key = SourceOverlay::key(path);
  for (size_t i = 0; i < modules.size(); ++i) {
    if (!modules[i].library && moduleKeys_[i] == key) {
      return i;
    }
  }

  return std::nullopt;
}

size_t Knowledge::symbolAt(size_t module, unsigned line, unsigned character,
                           const SourcePlace **place) const {
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (symbols[i].module == module && covers(symbols[i].place, line, character)) {
      if (place) {
        *place = &symbols[i].place;
      }
      return i;
    }
  }
  for (const KnownReference &reference : references) {
    if (reference.module == module && covers(reference.place, line, character)) {
      if (place) {
        *place = &reference.place;
      }
      return reference.symbol;
    }
  }

  return kNone;
}

size_t Knowledge::localAt(size_t module, unsigned line, unsigned character,
                          const SourcePlace **place) const {
  for (size_t i = 0; i < locals.size(); ++i) {
    const KnownLocal &local = locals[i];
    if (local.module != module) {
      continue;
    }
    if (covers(local.place, line, character)) {
      if (place) {
        *place = &local.place;
      }
      return i;
    }
    for (const SourcePlace &use : local.uses) {
      if (covers(use, line, character)) {
        if (place) {
          *place = &use;
        }
        return i;
      }
    }
  }

  return kNone;
}

const KnownType *Knowledge::typeAt(size_t module, unsigned line, unsigned character) const {
  for (const KnownType &type : types) {
    if (type.module == module && covers(type.place, line, character)) {
      return &type;
    }
  }

  return nullptr;
}

std::vector<size_t> Knowledge::at(size_t module, unsigned line,
                                  std::optional<unsigned> character) const {
  if (character) {
    size_t symbol = symbolAt(module, line, *character, nullptr);
    if (symbol != kNone) {
      return {symbol};
    }
    if (localAt(module, line, *character, nullptr) != kNone) {
      return {};
    }
  }
  size_t best = kNone;
  for (size_t i = 0; i < symbols.size(); ++i) {
    const SourcePlace &extent = symbols[i].extent;
    if (symbols[i].module != module || line < extent.line || line > extent.endLine) {
      continue;
    }
    if (best == kNone || size(extent) < size(symbols[best].extent)) {
      best = i;
    }
  }
  if (best != kNone) {
    return {best};
  }
  std::vector<size_t> used;
  for (const KnownReference &reference : references) {
    if (reference.module == module && reference.place.line == line &&
        !llvm::is_contained(used, reference.symbol)) {
      used.push_back(reference.symbol);
    }
  }

  return used;
}

}
