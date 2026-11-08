#include "compiler/lsp/lsp_features/lsp_features.h"
#include "llvm/ADT/StringExtras.h"
#include <map>
#include <set>

namespace tz {

namespace {

const char *const kKeywords[] = {
    "async",   "await",      "break",  "case",      "class",     "const",  "continue", "default",
    "do",      "else",       "enum",   "export",    "extends",   "false",  "for",      "function",
    "if",      "implements", "import", "in",        "interface", "let",    "new",      "of",
    "private", "protected",  "public", "readonly",  "return",    "static", "super",    "switch",
    "this",    "true",       "type",   "undefined", "while",     "yield"};

}

LspFeatures::LspFeatures(const Knowledge &knowledge, size_t module, llvm::StringRef text,
                         UriOf uriOf, TextOf textOf)
    : knowledge(knowledge),
      module_(module),
      text_(text),
      uriOf(uriOf),
      textOf(textOf) {}

bool LspFeatures::isWordChar(char c) {
  return llvm::isAlnum(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

size_t LspFeatures::offsetIn(llvm::StringRef text, unsigned line, unsigned character) {
  size_t at = 0;
  for (unsigned i = 0; i < line; ++i) {
    size_t next = text.find('\n', at);
    if (next == llvm::StringRef::npos) {
      return text.size();
    }
    at = next + 1;
  }
  unsigned units = 0;
  while (at < text.size() && text[at] != '\n' && units < character) {
    unsigned char byte = static_cast<unsigned char>(text[at]);
    size_t length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
    units += length == 4 ? 2 : 1;
    at += length;
  }

  return std::min(at, text.size());
}

int LspFeatures::symbolKind(llvm::StringRef kind) {
  if (kind == "method") {
    return 2;
  }
  if (kind == "function") {
    return 3;
  }
  if (kind == "constructor") {
    return 4;
  }
  if (kind == "field") {
    return 5;
  }
  if (kind == "class") {
    return 7;
  }
  if (kind == "type") {
    return 8;
  }
  if (kind == "enum") {
    return 13;
  }
  if (kind == "const") {
    return 21;
  }

  return 6;
}

std::string LspFeatures::code(llvm::StringRef text) {
  return "```ts\n" + text.str() + "\n```";
}

bool LspFeatures::isName(llvm::StringRef name) {
  if (name.empty() || llvm::isDigit(name.front())) {
    return false;
  }

  return llvm::all_of(name, [](char c) { return LspFeatures::isWordChar(c); });
}

llvm::json::Object LspFeatures::range(const SourcePlace &place) {
  llvm::json::Object start{{"line", place.line}, {"character", place.character}};
  llvm::json::Object end{{"line", place.endLine}, {"character", place.endCharacter}};

  return llvm::json::Object{{"start", std::move(start)}, {"end", std::move(end)}};
}

std::string LspFeatures::uriOfPlace(const SourcePlace &place) const {
  if (!place.file) {
    for (const KnownModule &module : this->knowledge.modules) {
      if (module.library && module.path == place.path && !module.file.empty()) {
        return this->uriOf(module.file);
      }
    }
  }

  return this->uriOf(place.path);
}

llvm::json::Object LspFeatures::location(const SourcePlace &place) const {
  return llvm::json::Object{{"uri", uriOfPlace(place)}, {"range", range(place)}};
}

size_t LspFeatures::offset(unsigned line, unsigned character) const {
  return LspFeatures::offsetIn(text_, line, character);
}

llvm::json::Value LspFeatures::hover(unsigned line, unsigned character) const {
  const SourcePlace *at = nullptr;
  std::string value;
  size_t symbol = this->knowledge.symbolAt(module_, line, character, &at);
  if (symbol != Knowledge::kNone) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    std::string title = known.kind + " " + this->knowledge.qualified(symbol);
    value = LspFeatures::code(known.signature.empty() ? title : known.signature) + "\n\n" + title +
            " - " + this->knowledge.modules[known.module].name;
  } else if (size_t local = this->knowledge.localAt(module_, line, character, &at);
             local != Knowledge::kNone) {
    const KnownLocal &known = this->knowledge.locals[local];
    value = LspFeatures::code(known.name + ": " + known.type);
  } else if (const KnownType *type = this->knowledge.typeAt(module_, line, character)) {
    at = &type->place;
    value = LspFeatures::code(type->kind == "class" ? "class " + type->name
                                                    : type->name + ": " + type->type);
  } else {
    return nullptr;
  }
  llvm::json::Object contents{{"kind", "markdown"}, {"value", value}};

  return llvm::json::Object{{"contents", std::move(contents)}, {"range", range(*at)}};
}

llvm::json::Value LspFeatures::definition(unsigned line, unsigned character) const {
  size_t symbol = this->knowledge.symbolAt(module_, line, character, nullptr);
  if (symbol != Knowledge::kNone) {
    return location(this->knowledge.symbols[symbol].place);
  }
  size_t local = this->knowledge.localAt(module_, line, character, nullptr);
  if (local != Knowledge::kNone) {
    return location(this->knowledge.locals[local].place);
  }

  return nullptr;
}

std::vector<SourcePlace> LspFeatures::occurrences(unsigned line, unsigned character,
                                                  bool declaration, std::string &name,
                                                  bool &library) const {
  std::vector<SourcePlace> places;
  size_t symbol = this->knowledge.symbolAt(module_, line, character, nullptr);
  if (symbol != Knowledge::kNone) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    name = known.name;
    library = this->knowledge.modules[known.module].library;
    if (declaration) {
      places.push_back(known.place);
    }
    for (size_t reference : this->knowledge.referencesTo(symbol)) {
      places.push_back(this->knowledge.references[reference].place);
    }
    return places;
  }
  size_t local = this->knowledge.localAt(module_, line, character, nullptr);
  if (local != Knowledge::kNone) {
    const KnownLocal &known = this->knowledge.locals[local];
    name = known.name;
    library = this->knowledge.modules[known.module].library;
    if (declaration) {
      places.push_back(known.place);
    }
    places.insert(places.end(), known.uses.begin(), known.uses.end());
  }

  return places;
}

llvm::json::Value LspFeatures::references(unsigned line, unsigned character,
                                          bool declaration) const {
  std::string name;
  bool library = false;
  llvm::json::Array found;
  for (const SourcePlace &place : occurrences(line, character, declaration, name, library)) {
    found.push_back(location(place));
  }

  return found;
}

llvm::json::Value LspFeatures::prepareRename(unsigned line, unsigned character) const {
  const SourcePlace *at = nullptr;
  std::string name;
  size_t symbol = this->knowledge.symbolAt(module_, line, character, &at);
  bool library = false;
  if (symbol != Knowledge::kNone) {
    name = this->knowledge.symbols[symbol].name;
    library = this->knowledge.modules[this->knowledge.symbols[symbol].module].library;
  } else if (size_t local = this->knowledge.localAt(module_, line, character, &at);
             local != Knowledge::kNone) {
    name = this->knowledge.locals[local].name;
  }
  if (!at || library || name == "constructor") {
    return nullptr;
  }

  return llvm::json::Object{{"range", range(*at)}, {"placeholder", name}};
}

llvm::json::Value LspFeatures::rename(unsigned line, unsigned character, llvm::StringRef name,
                                      std::string &error) const {
  if (!isName(name)) {
    error = "'" + name.str() + "' is not a name";
    return nullptr;
  }
  std::string old;
  bool library = false;
  std::vector<SourcePlace> places = occurrences(line, character, true, old, library);
  if (places.empty() || library || old == "constructor") {
    error = "this cannot be renamed: only what the program declares can";
    return nullptr;
  }
  std::map<std::string, std::string> texts;
  std::map<std::string, llvm::json::Array> changes;
  std::set<std::tuple<std::string, unsigned, unsigned>> seen;
  for (const SourcePlace &place : places) {
    if (!place.file || !seen.insert({place.path, place.line, place.character}).second) {
      continue;
    }
    auto found = texts.find(place.path);
    if (found == texts.end()) {
      found = texts.try_emplace(place.path, this->textOf(place.path)).first;
    }
    llvm::StringRef text = found->second;
    size_t start = LspFeatures::offsetIn(text, place.line, place.character);
    if (!text.substr(start).starts_with(old) ||
        (start + old.size() < text.size() && LspFeatures::isWordChar(text[start + old.size()]))) {
      continue;
    }
    SourcePlace exact = place;
    exact.endLine = place.line;
    exact.endCharacter = place.character + static_cast<unsigned>(old.size());
    changes[this->uriOf(place.path)].push_back(
        llvm::json::Object{{"range", range(exact)}, {"newText", name.str()}});
  }
  llvm::json::Object all;
  for (auto &[uri, edits] : changes) {
    all[uri] = std::move(edits);
  }

  return llvm::json::Object{{"changes", std::move(all)}};
}

llvm::json::Object LspFeatures::legend() {
  llvm::json::Array types{"class",    "type",     "enum",      "function",  "method",
                          "property", "variable", "parameter", "enumMember"};
  llvm::json::Array modifiers{"declaration", "readonly", "defaultLibrary"};

  return llvm::json::Object{{"tokenTypes", std::move(types)},
                            {"tokenModifiers", std::move(modifiers)}};
}

llvm::json::Value LspFeatures::semanticTokens() const {
  struct Token {
    unsigned line;
    unsigned character;
    unsigned length;
    int type;
    int modifiers;
  };
  auto typeOf = [](llvm::StringRef kind) {
    if (kind == "class" || kind == "constructor") {
      return 0;
    }
    if (kind == "type") {
      return 1;
    }
    if (kind == "enum") {
      return 2;
    }
    if (kind == "function") {
      return 3;
    }
    if (kind == "method") {
      return 4;
    }
    if (kind == "field") {
      return 5;
    }
    if (kind == "enumMember") {
      return 8;
    }

    return 6;
  };
  std::vector<std::pair<size_t, size_t>> imports;
  for (size_t start = 0; start < text_.size();) {
    size_t end = text_.find('\n', start);
    end = end == llvm::StringRef::npos ? text_.size() : end + 1;
    llvm::StringRef line = text_.slice(start, end);
    llvm::StringRef code = line.ltrim();
    bool names = code.starts_with("import") && code.size() > 6 && !LspFeatures::isWordChar(code[6]);
    if (!names && code.starts_with("export") && code.size() > 6 &&
        !LspFeatures::isWordChar(code[6])) {
      llvm::StringRef after = code.drop_front(6).ltrim();
      names = after.starts_with("{") || after.starts_with("*") || after.starts_with("type {");
    }
    if (!names) {
      start = end;
      continue;
    }
    size_t from = start + (line.size() - code.size());
    size_t quote = text_.find_first_of("'\";", from);
    if (quote == llvm::StringRef::npos || text_[quote] == ';') {
      imports.push_back({from, quote == llvm::StringRef::npos ? text_.size() : quote});
      start = end;
      continue;
    }
    size_t close = text_.find(text_[quote], quote + 1);
    size_t stop = close == llvm::StringRef::npos ? text_.size() : close + 1;
    imports.push_back({from, stop});
    size_t next = text_.find('\n', stop);
    start = next == llvm::StringRef::npos ? text_.size() : next + 1;
  }
  std::map<std::pair<unsigned, unsigned>, Token> found;
  auto add = [&](const SourcePlace &place, llvm::StringRef name, int type, int modifiers) {
    if (place.endLine != place.line || name.empty() || name == "this") {
      return;
    }
    size_t at = offset(place.line, place.character);
    if (llvm::any_of(imports, [&](const std::pair<size_t, size_t> &span) {
          return at >= span.first && at < span.second;
        })) {
      return;
    }
    llvm::StringRef rest = text_.substr(at);
    if (!rest.starts_with(name) ||
        (rest.size() > name.size() && LspFeatures::isWordChar(rest[name.size()]))) {
      return;
    }
    auto [slot, added] = found.try_emplace(
        {place.line, place.character},
        Token{place.line, place.character, place.endCharacter - place.character, type, modifiers});
    if (!added && type < slot->second.type) {
      slot->second.type = type;
    }
  };
  auto modifiersOf = [&](size_t symbol, bool declaration) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    int modifiers = declaration ? 1 : 0;
    if (known.kind == "const" || llvm::StringRef(known.signature).contains("readonly ")) {
      modifiers |= 2;
    }
    if (this->knowledge.modules[known.module].library) {
      modifiers |= 4;
    }

    return modifiers;
  };
  for (size_t symbol : this->knowledge.symbolsOf(module_)) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    llvm::StringRef name = known.kind == "constructor" ? llvm::StringRef() : known.name;
    add(known.place, name, typeOf(known.kind), modifiersOf(symbol, true));
  }
  for (const KnownReference &reference : this->knowledge.references) {
    if (reference.module != module_) {
      continue;
    }
    const KnownSymbol &known = this->knowledge.symbols[reference.symbol];
    llvm::StringRef name = known.kind == "constructor" ? known.container : known.name;
    add(reference.place, name, typeOf(known.kind), modifiersOf(reference.symbol, false));
  }
  for (const KnownType &builtin : this->knowledge.types) {
    if (builtin.module == module_ && builtin.kind == "class") {
      add(builtin.place, builtin.name, 0, 4);
    }
  }
  for (const KnownLocal &local : this->knowledge.locals) {
    if (local.module != module_) {
      continue;
    }
    int type = local.kind == "parameter" ? 7 : 6;
    int readonly = local.kind == "const" ? 2 : 0;
    add(local.place, local.name, type, 1 | readonly);
    for (const SourcePlace &use : local.uses) {
      add(use, local.name, type, readonly);
    }
  }
  llvm::json::Array data;
  unsigned line = 0;
  unsigned character = 0;
  for (const auto &[at, token] : found) {
    unsigned deltaLine = token.line - line;
    unsigned deltaCharacter = deltaLine == 0 ? token.character - character : token.character;
    data.push_back(deltaLine);
    data.push_back(deltaCharacter);
    data.push_back(token.length);
    data.push_back(token.type);
    data.push_back(token.modifiers);
    line = token.line;
    character = token.character;
  }

  return llvm::json::Object{{"data", std::move(data)}};
}

llvm::json::Value LspFeatures::completion(unsigned line, unsigned character) const {
  size_t at = offset(line, character);
  size_t start = at;
  while (start > 0 && LspFeatures::isWordChar(text_[start - 1])) {
    --start;
  }
  llvm::json::Array items;
  std::set<std::string> labels;
  auto add = [&](llvm::StringRef label, int kind, llvm::StringRef detail) {
    if (!label.empty() && labels.insert(label.str()).second) {
      items.push_back(
          llvm::json::Object{{"label", label.str()}, {"kind", kind}, {"detail", detail.str()}});
    }
  };
  if (start > 0 && text_[start - 1] == '.') {
    size_t end = start - 1;
    if (end > 0 && text_[end - 1] == '?') {
      --end;
    }
    size_t from = end;
    while (from > 0 && LspFeatures::isWordChar(text_[from - 1])) {
      --from;
    }
    std::string object = text_.slice(from, end).str();
    std::vector<KnownMember> members;
    if (object == "this") {
      std::vector<size_t> around = this->knowledge.at(module_, line, std::nullopt);
      size_t owner = around.empty() ? Knowledge::kNone : around.front();
      while (owner != Knowledge::kNone && this->knowledge.symbols[owner].kind != "class") {
        owner = this->knowledge.symbols[owner].parent;
      }
      if (owner != Knowledge::kNone) {
        for (size_t member : this->knowledge.members(owner)) {
          const KnownSymbol &known = this->knowledge.symbols[member];
          if (known.kind != "constructor") {
            members.push_back({known.name, known.kind, known.signature});
          }
        }
      }
    } else {
      const KnownType *best = nullptr;
      for (const KnownType &type : this->knowledge.types) {
        bool before =
            type.place.line < line || (type.place.line == line && type.place.character < character);
        if (type.module == module_ && type.name == object && before) {
          best = &type;
        }
      }
      if (!best) {
        for (const KnownType &type : this->knowledge.types) {
          if (type.module == module_ && type.name == object) {
            best = &type;
          }
        }
      }
      if (best) {
        auto found = this->knowledge.typeMembers.find(best->base);
        if (found != this->knowledge.typeMembers.end()) {
          members = found->second;
        }
      }
    }
    for (const KnownMember &member : members) {
      add(member.name, LspFeatures::symbolKind(member.kind), member.detail);
    }
    return llvm::json::Object{{"isIncomplete", false}, {"items", std::move(items)}};
  }

  std::vector<size_t> around = this->knowledge.at(module_, line, std::nullopt);
  size_t owner = around.empty() ? Knowledge::kNone : around.front();
  for (const KnownLocal &local : this->knowledge.locals) {
    if (local.module == module_ && (local.owner == owner || local.owner == Knowledge::kNone)) {
      add(local.name, 6, local.type);
    }
  }
  for (size_t symbol : this->knowledge.symbolsOf(module_)) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    if (known.parent == Knowledge::kNone) {
      add(known.name, LspFeatures::symbolKind(known.kind), known.signature);
    }
  }
  for (const KnownReference &reference : this->knowledge.references) {
    const KnownSymbol &known = this->knowledge.symbols[reference.symbol];
    if (reference.module == module_ && known.parent == Knowledge::kNone) {
      add(known.name, LspFeatures::symbolKind(known.kind), known.signature);
    }
  }
  for (const char *keyword : kKeywords) {
    add(keyword, 14, "");
  }

  return llvm::json::Object{{"isIncomplete", false}, {"items", std::move(items)}};
}

}
