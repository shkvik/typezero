#include "compiler/packages/port/file_port/file_port.h"
#include "compiler/packages/port/edit/edit.h"
#include "compiler/packages/port/note/note.h"
#include "compiler/packages/port/port_names/port_names.h"
#include "compiler/packages/scanner/scanner.h"
#include "compiler/packages/specifier/specifier.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <set>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

bool FilePort::isOperandEnd(const Token &t) {
  switch (t.kind) {
  case Tok::Identifier:
  case Tok::IntLiteral:
  case Tok::FloatLiteral:
  case Tok::StringLiteral:
  case Tok::TemplateString:
  case Tok::TemplateTail:
  case Tok::RegexLiteral:
  case Tok::RParen:
  case Tok::RBracket:
  case Tok::RBrace:
  case Tok::KwTrue:
  case Tok::KwFalse:
    return true;
  default:
    return false;
  }
}

std::string FilePort::templateText(llvm::StringRef literal) {
  llvm::StringRef raw = literal.drop_front().drop_back();
  std::string out;
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\\' && i + 1 < raw.size()) {
      out += c;
      out += raw[++i];
    } else if (c == '`') {
      out += "\\`";
    } else if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
      out += "\\$";
    } else {
      out += c;
    }
  }

  return out;
}

void FilePort::retokenize() {
  this->toks = Scanner::tokenize(text_);
  this->close.assign(this->toks.size(), std::string::npos);
  std::vector<size_t> open;
  for (size_t i = 0; i < this->toks.size(); ++i) {
    Tok k = this->toks[i].kind;
    if (k == Tok::LParen || k == Tok::LBracket || k == Tok::LBrace || k == Tok::TemplateHead) {
      open.push_back(i);
      continue;
    }
    Tok want = k == Tok::RParen         ? Tok::LParen
               : k == Tok::RBracket     ? Tok::LBracket
               : k == Tok::RBrace       ? Tok::LBrace
               : k == Tok::TemplateTail ? Tok::TemplateHead
                                        : Tok::Eof;
    if (want == Tok::Eof) {
      continue;
    }
    if (!open.empty() && this->toks[open.back()].kind == want) {
      this->close[open.back()] = i;
      open.pop_back();
    }
  }
}

bool FilePort::apply() {
  if (this->edits.empty()) {
    return false;
  }
  std::stable_sort(this->edits.begin(), this->edits.end(),
                   [](const Edit &a, const Edit &b) { return a.begin < b.begin; });
  std::vector<const Edit *> taken;
  size_t last = 0;
  for (const Edit &edit : this->edits) {
    if (edit.begin < last) {
      continue;
    }
    taken.push_back(&edit);
    last = std::max(last, edit.end);
  }
  auto shift = [&](size_t offset) {
    long long delta = 0;
    for (const Edit *edit : taken) {
      if (edit->begin >= offset && !(edit->begin == offset && edit->end == offset)) {
        break;
      }
      if (offset < edit->end) {
        return static_cast<size_t>(static_cast<long long>(edit->begin) + delta);
      }
      delta += static_cast<long long>(edit->text.size()) -
               static_cast<long long>(edit->end - edit->begin);
    }

    return static_cast<size_t>(static_cast<long long>(offset) + delta);
  };
  for (Note &n : notes_) {
    n.offset = shift(n.offset);
  }
  std::string out;
  size_t from = 0;
  long long delta = 0;
  for (const Edit *edit : taken) {
    out.append(text_, from, edit->begin - from);
    if (!edit->note.empty()) {
      notes_.push_back(
          {static_cast<size_t>(static_cast<long long>(edit->begin) + delta), edit->note});
    }
    out += edit->text;
    delta +=
        static_cast<long long>(edit->text.size()) - static_cast<long long>(edit->end - edit->begin);
    from = edit->end;
    if (edit->kind) {
      ++this->counts[edit->kind];
    }
  }
  out.append(text_, from, std::string::npos);
  text_ = std::move(out);
  this->edits.clear();

  return true;
}

void FilePort::run() {
  retokenize();
  modules();
  if (apply()) {
    retokenize();
  }
  streamHooks();
  if (apply()) {
    retokenize();
  }
  for (int pass = 0; pass < 8; ++pass) {
    tokens();
    concatenations();
    if (!apply()) {
      break;
    }
    retokenize();
  }
  this->fieldValues();
  if (apply()) {
    retokenize();
  }
  manual();
  std::stable_sort(notes_.begin(), notes_.end(),
                   [](const Note &a, const Note &b) { return a.offset < b.offset; });
}

void FilePort::streamHooks() {
  static const llvm::StringLiteral streams[] = {"Readable", "Writable", "Duplex", "Transform",
                                                "PassThrough"};
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> hooks[] = {
      {"_read", "onRead"},           {"_write", "onWrite"},        {"_writev", "onWritev"},
      {"_transform", "onTransform"}, {"_flush", "onFlush"},        {"_final", "onFinal"},
      {"_destroy", "onDestroy"},     {"_construct", "onConstruct"}};
  const char *kind = "'_read', '_write' and the other methods of a stream became 'onRead', "
                     "'onWrite'...";
  for (size_t i = 0; i < this->toks.size(); ++i) {
    if (!isName(i, "class") || afterDot(i)) {
      continue;
    }
    size_t open = i + 1;
    llvm::StringRef base;
    bool extends = false;
    while (open < this->toks.size() && !is(open, Tok::LBrace)) {
      if (isName(open, "extends")) {
        extends = true;
      } else if (extends && base.empty() && is(open, Tok::Identifier) && !is(open + 1, Tok::Dot)) {
        base = this->toks[open].text;
      }
      ++open;
    }
    if (open >= this->toks.size() || this->close[open] == std::string::npos ||
        !llvm::is_contained(streams, base)) {
      continue;
    }
    for (size_t k = open + 1; k < this->close[open]; ++k) {
      if (!is(k, Tok::Identifier)) {
        continue;
      }
      const auto *hook =
          llvm::find_if(hooks, [&](const auto &h) { return h.first == this->toks[k].text; });
      if (hook == std::end(hooks)) {
        continue;
      }
      bool member = !afterDot(k) && is(k + 1, Tok::LParen);
      bool own = afterDot(k) && k >= 2 && (isName(k - 2, "this") || isName(k - 2, "super"));
      if (member || own) {
        add(at(k), endOf(k), hook->second.str(), kind);
      }
    }
  }
}

void FilePort::modules() {
  for (size_t i = 0; i < this->toks.size(); ++i) {
    if (afterDot(i)) {
      continue;
    }
    if (isName(i, "export")) {
      if (is(i + 1, Tok::KwDefault)) {
        exportDefault(i);
      } else if (isName(i + 1, "type") && is(i + 2, Tok::LBrace)) {
        add(at(i + 1), at(i + 2), "", "'import type' and 'export type' lost 'type'");
        exportList(i + 2, this->close[i + 2]);
      } else if (isName(i + 1, "type") && is(i + 2, Tok::Star)) {
        add(at(i + 1), at(i + 2), "", "'import type' and 'export type' lost 'type'");
      } else if (is(i + 1, Tok::LBrace)) {
        exportList(i + 1, this->close[i + 1]);
      }
    } else if (isName(i, "import")) {
      importDecl(i);
    }
  }
  for (size_t i = 1; i < this->toks.size(); ++i) {
    if (!is(i, Tok::StringLiteral) ||
        !(isName(i - 1, "from") || (isName(i - 1, "import") && !afterDot(i - 1)))) {
      continue;
    }
    std::string spec = Scanner::stringValue(this->toks[i]);
    llvm::StringRef ref(spec);
    llvm::StringRef ext = path::extension(ref, path::Style::posix);
    if (!(ref.starts_with("./") || ref.starts_with("../")) || ref.ends_with(".d.ts") ||
        (ext != ".ts" && ext != ".js")) {
      continue;
    }
    char quote = this->toks[i].text.front();
    add(at(i), endOf(i), quote + ref.drop_back(3).str() + ".tz" + quote,
        "imports of .ts and .js files name the .tz files of the copy");
  }
}

void FilePort::exportDefault(size_t i) {
  const char *kind = "'export default' became a named export";
  size_t def = i + 1, k = i + 2;
  if (k >= this->toks.size()) {
    return;
  }
  llvm::SmallString<256> real;
  fs::real_path(file_, real);
  auto known = this->ctx.defaultNames.find(std::string(real));
  std::string name = known != this->ctx.defaultNames.end() ? known->second : "";
  size_t keyword = k;
  if (isName(keyword, "async") && is(keyword + 1, Tok::KwFunction)) {
    ++keyword;
  }
  if (isName(keyword, "abstract") && isName(keyword + 1, "class")) {
    ++keyword;
  }
  if (is(keyword, Tok::KwFunction) || isName(keyword, "class") || isName(keyword, "interface")) {
    size_t last = keyword;
    if (is(last + 1, Tok::Star)) {
      ++last;
    }
    bool named = is(last + 1, Tok::Identifier) && this->toks[last + 1].text != "extends" &&
                 this->toks[last + 1].text != "implements";
    if (named) {
      add(at(def), at(k), "", kind);
    } else {
      std::string words = text_.substr(at(k), endOf(last) - at(k));
      if (last + 1 < this->toks.size()) {
        add(at(def), at(last + 1), words + " " + name + (is(last + 1, Tok::LParen) ? "" : " "),
            kind);
      } else {
        add(at(def), endOf(last), words + " " + name, kind);
      }
    }
    return;
  }
  if (is(k, Tok::Identifier) &&
      (k + 1 >= this->toks.size() || is(k + 1, Tok::Semi) || this->toks[k + 1].newlineBefore)) {
    add(at(i), endOf(k), "export { " + this->toks[k].text.str() + " }", kind);
    return;
  }
  add(at(def), endOf(def), "const " + name + " =", kind);
}

void FilePort::exportList(size_t open, size_t close) {
  if (close == std::string::npos) {
    return;
  }
  std::string target;
  if (isName(close + 1, "from") && is(close + 2, Tok::StringLiteral)) {
    std::string spec = Scanner::stringValue(this->toks[close + 2]);
    std::string file = Scanner::resolveRelative(file_, spec, Sources::TypeScript);
    llvm::SmallString<256> real;
    if (!file.empty() && !fs::real_path(file, real)) {
      if (auto it = this->ctx.defaultNames.find(std::string(real));
          it != this->ctx.defaultNames.end()) {
        target = it->second;
      }
    }
  }
  for (size_t j = open + 1; j < close; ++j) {
    if (isName(j, "type") && is(j + 1, Tok::Identifier) && !is(j + 1, Tok::Comma)) {
      add(at(j), at(j + 1), "", "'import type' and 'export type' lost 'type'");
    }
    if (is(j, Tok::KwAs) && is(j + 1, Tok::KwDefault)) {
      add(endOf(j - 1), endOf(j + 1), "", "'export default' became a named export");
      ++j;
    } else if (is(j, Tok::KwDefault) && !target.empty()) {
      bool renamed = is(j + 1, Tok::KwAs) && is(j + 2, Tok::KwDefault);
      add(at(j), renamed ? endOf(j + 2) : endOf(j), target,
          "'export default' became a named export");
      if (renamed) {
        j += 2;
      }
    }
  }
}

bool FilePort::members(const std::string &name, size_t skipBegin, size_t skipEnd,
                       std::vector<std::string> &items) {
  llvm::StringSet<> own;
  for (size_t j = 0; j < this->toks.size(); ++j) {
    if ((j < skipBegin || j >= skipEnd) && is(j, Tok::Identifier) && !afterDot(j)) {
      own.insert(this->toks[j].text);
    }
  }
  std::vector<Edit> uses;
  std::set<std::string> seen;
  for (size_t j = 0; j < this->toks.size(); ++j) {
    if (j >= skipBegin && j < skipEnd) {
      continue;
    }
    if (!isName(j, name) || afterDot(j)) {
      continue;
    }
    if (is(j + 1, Tok::Colon) && j > 0 && (is(j - 1, Tok::LBrace) || is(j - 1, Tok::Comma))) {
      continue;
    }
    if (!is(j + 1, Tok::Dot) || !is(j + 2, Tok::Identifier)) {
      return false;
    }
    std::string member = this->toks[j + 2].text.str();
    std::string local = own.contains(member) ? name + PortNames::capitalize(member) : member;
    uses.push_back({at(j), endOf(j + 2), local, nullptr, ""});
    if (seen.insert(member).second) {
      items.push_back(local == member ? member : member + " as " + local);
    }
  }
  for (Edit &use : uses) {
    this->edits.push_back(std::move(use));
  }

  return true;
}

void FilePort::importDecl(size_t i) {
  if (is(i + 1, Tok::StringLiteral) || is(i + 1, Tok::LParen)) {
    return;
  }
  size_t from = std::string::npos;
  for (size_t j = i + 1; j < this->toks.size() && j < i + 2000; ++j) {
    if (is(j, Tok::LBrace) && this->close[j] != std::string::npos) {
      j = this->close[j];
      continue;
    }
    if (is(j, Tok::Semi) || is(j, Tok::Assign)) {
      return;
    }
    if (isName(j, "from") && is(j + 1, Tok::StringLiteral)) {
      from = j;
      break;
    }
  }
  if (from == std::string::npos) {
    return;
  }
  const char *typeKind = "'import type' and 'export type' lost 'type'";
  size_t c = i + 1;
  if (isName(c, "type") && !isName(c + 1, "from") && !is(c + 1, Tok::Comma)) {
    add(at(c), at(c + 1), "", typeKind);
    ++c;
  }
  size_t clauseBegin = c;
  std::string spec = Scanner::stringValue(this->toks[from + 1]);
  llvm::StringRef specRef(spec);

  std::string package, source, sub;
  if (Specifier::splitJsrImport(specRef, package, source, sub)) {
    char quote = this->toks[from + 1].text.front();
    add(at(from + 1), endOf(from + 1), std::string(1, quote) + package + sub + quote,
        "'jsr:' imports became package names in package.json");
  }

  std::string defaultName, ns;
  if (is(c, Tok::Identifier) && (is(c + 1, Tok::Comma) || c + 1 == from)) {
    defaultName = this->toks[c].text.str();
    c += is(c + 1, Tok::Comma) ? 2 : 1;
  }
  if (is(c, Tok::Star) && is(c + 1, Tok::KwAs) && is(c + 2, Tok::Identifier)) {
    ns = this->toks[c + 2].text.str();
    c += 3;
  }
  std::vector<std::string> items;
  if (is(c, Tok::LBrace) && this->close[c] != std::string::npos) {
    bool rewrite = !defaultName.empty() || !ns.empty();
    std::string item;
    for (size_t j = c + 1; j < this->close[c]; ++j) {
      if (isName(j, "type") && is(j + 1, Tok::Identifier)) {
        if (!rewrite) {
          add(at(j), at(j + 1), "", typeKind);
        } else {
          ++this->counts[typeKind];
        }
        continue;
      }
      if (is(j, Tok::Comma)) {
        if (!item.empty()) {
          items.push_back(item);
        }
        item.clear();
        continue;
      }
      item += (item.empty() ? "" : " ") + this->toks[j].text.str();
    }
    if (!item.empty()) {
      items.push_back(item);
    }
  }
  if (defaultName.empty() && ns.empty()) {
    return;
  }

  bool relative = specRef.starts_with(".");
  if (!defaultName.empty()) {
    std::string exported;
    if (relative) {
      std::string target = Scanner::resolveRelative(file_, spec, Sources::TypeScript);
      llvm::SmallString<256> real;
      if (!target.empty() && !fs::real_path(target, real)) {
        if (auto it = this->ctx.defaultNames.find(std::string(real));
            it != this->ctx.defaultNames.end()) {
          exported = it->second;
        }
      }
      if (exported.empty()) {
        notes_.push_back({at(clauseBegin), "'" + spec +
                                               "' has no default export that tz port renamed; "
                                               "import what it exports by name"});
        return;
      }
      items.push_back(exported == defaultName ? defaultName : exported + " as " + defaultName);
    } else if (!members(defaultName, i, from + 2, items)) {
      notes_.push_back({at(clauseBegin), "a default import of a module is not supported, and '" +
                                             defaultName +
                                             "' is used as a whole: import by name what it uses"});
      return;
    }
  }
  if (!ns.empty() && !members(ns, i, from + 2, items)) {
    notes_.push_back({at(clauseBegin), "'import * as " + ns + "' is not supported, and '" + ns +
                                           "' is used as a whole: import by name what it uses"});
    return;
  }
  add(at(clauseBegin), at(from), "{ " + llvm::join(items, ", ") + " } ",
      !ns.empty() ? "'import * as' became named imports" : "default imports became named imports");
}

void FilePort::tokens() {
  for (size_t i = 0; i < this->toks.size(); ++i) {
    const Token &t = this->toks[i];
    switch (t.kind) {
    case Tok::EqEqEq:
      add(at(i), endOf(i), "==", "'===' and '!==' became '==' and '!='");
      break;
    case Tok::BangEqEq:
      add(at(i), endOf(i), "!=", "'===' and '!==' became '==' and '!='");
      break;
    case Tok::KwVar:
      add(at(i), endOf(i), "let", "'var' became 'let'");
      break;
    case Tok::KwVoid:
      if (is(i + 1, Tok::IntLiteral) && this->toks[i + 1].text == "0") {
        add(at(i), endOf(i + 1), "undefined", "'null' and 'void 0' became 'undefined'");
      }
      break;
    case Tok::Identifier:
      if (t.hash) {
        privateName(i);
        break;
      }
      if (afterDot(i)) {
        break;
      }
      if (t.text == "null") {
        if (is(i + 1, Tok::Colon) && i > 0 && (is(i - 1, Tok::LBrace) || is(i - 1, Tok::Comma))) {
          break;
        }
        if (i >= 2 && is(i - 1, Tok::LParen) && isName(i - 2, "push") && afterDot(i - 2) &&
            is(i + 1, Tok::RParen)) {
          add(at(i - 2), endOf(i + 1), "pushEnd()", "'push(null)' became 'pushEnd()'");
          break;
        }
        const char *json = "'| null' next to Json was dropped: a Json holds null itself";
        if (i >= 2 && is(i - 1, Tok::Pipe) && isName(i - 2, "Json")) {
          add(endOf(i - 2), endOf(i), "", json);
          break;
        }
        if (is(i + 1, Tok::Pipe) && isName(i + 2, "Json")) {
          add(at(i), at(i + 2), "", json);
          break;
        }
        if (i >= 2 && is(i - 1, Tok::Assign) && isName(i - 2, "Json")) {
          break;
        }
        const char *kind = "'null' and 'void 0' became 'undefined'";
        if (i >= 2 && is(i - 1, Tok::Pipe) && isName(i - 2, "undefined")) {
          add(endOf(i - 2), endOf(i), "", kind);
        } else if (is(i + 1, Tok::Pipe) && isName(i + 2, "undefined")) {
          add(at(i), at(i + 2), "", kind);
        } else {
          add(at(i), endOf(i), "undefined", kind);
        }
      } else if (t.text == "any" && i > 0 && !is(i + 1, Tok::Colon) && !is(i + 1, Tok::Assign) &&
                 !is(i + 1, Tok::LParen) && !is(i + 1, Tok::Dot) &&
                 (is(i - 1, Tok::Colon) || is(i - 1, Tok::Less) || is(i - 1, Tok::Comma) ||
                  is(i - 1, Tok::Pipe) || is(i - 1, Tok::Amp) || is(i - 1, Tok::Arrow) ||
                  is(i - 1, Tok::LParen) || is(i - 1, Tok::LBracket) || is(i - 1, Tok::KwAs) ||
                  isName(i - 1, "extends") || isName(i - 1, "keyof") || isName(i - 1, "readonly") ||
                  isName(i - 1, "is") ||
                  (is(i - 1, Tok::Assign) && i >= 3 && isName(i - 3, "type")))) {
        add(at(i), endOf(i), "unknown", "'any' became 'unknown'",
            "'any' became 'unknown': give the exact type, or narrow the value before using it "
            "(typeof, instanceof, 'in')");
      }
      break;
    default:
      break;
    }
  }
}

size_t FilePort::unary(size_t i) const {
  size_t n = this->toks.size();
  while (i < n && (is(i, Tok::Bang) || is(i, Tok::Tilde) || is(i, Tok::Minus) || is(i, Tok::Plus) ||
                   is(i, Tok::KwTypeof) || is(i, Tok::KwVoid) || is(i, Tok::KwDelete) ||
                   isName(i, "await"))) {
    ++i;
  }
  if (i >= n) {
    return 0;
  }
  auto closed = [&](size_t j) {
    return this->close[j] == std::string::npos ? 0 : this->close[j] + 1;
  };
  switch (this->toks[i].kind) {
    case Tok::Identifier: {
      llvm::StringRef name = this->toks[i].text;
      if (name == "function" || name == "class" || name == "async" || name == "yield" ||
          is(i + 1, Tok::Arrow)) {
        return 0;
      }
      ++i;
      break;
    }
    case Tok::KwNew:
      if (!is(i + 1, Tok::Identifier)) {
        return 0;
      }
      i += 2;
      break;
    case Tok::IntLiteral:
    case Tok::FloatLiteral:
    case Tok::StringLiteral:
    case Tok::TemplateString:
    case Tok::RegexLiteral:
    case Tok::KwTrue:
    case Tok::KwFalse:
      ++i;
      break;
    case Tok::TemplateHead:
    case Tok::LBracket:
      i = closed(i);
      break;
    case Tok::LParen: {
      size_t after = closed(i);
      if (!after || is(after, Tok::Arrow)) {
        return 0;
      }
      i = after;
      break;
    }
    default:
      return 0;
  }
  while (i && i < n) {
    const Token &t = this->toks[i];
    if (t.is(Tok::Dot) || t.is(Tok::QuestionDot)) {
      if (is(i + 1, Tok::Identifier)) {
        i += 2;
      } else if (t.is(Tok::QuestionDot) && (is(i + 1, Tok::LParen) || is(i + 1, Tok::LBracket))) {
        i = closed(i + 1);
      } else {
        return 0;
      }
    } else if (t.is(Tok::LParen) || t.is(Tok::LBracket) || t.is(Tok::TemplateHead)) {
      i = closed(i);
    } else if (t.is(Tok::TemplateString) ||
               ((t.is(Tok::Bang) || t.is(Tok::PlusPlus) || t.is(Tok::MinusMinus)) &&
                !t.newlineBefore)) {
      ++i;
    } else {
      break;
    }
  }

  return i;
}

size_t FilePort::operand(size_t i) const {
  size_t end = unary(i);
  while (end && (is(end, Tok::Star) || is(end, Tok::Slash) || is(end, Tok::Percent) ||
                 is(end, Tok::StarStar))) {
    end = unary(end + 1);
  }

  return end;
}

void FilePort::privateName(size_t i) {
  const char *kind = "'#x' became 'private x'";
  size_t first = i;
  while (first > 0 && (isName(first - 1, "static") || isName(first - 1, "readonly") ||
                       isName(first - 1, "async") || isName(first - 1, "override"))) {
    --first;
  }
  bool declared = !afterDot(i) &&
                  (first == 0 || this->toks[first].newlineBefore || is(first - 1, Tok::LBrace) ||
                   is(first - 1, Tok::RBrace) || is(first - 1, Tok::Semi));
  if (!declared) {
    add(at(i) - 1, at(i), "", kind);
  } else if (first == i) {
    add(at(i) - 1, at(i), "private ", kind);
  } else {
    add(at(first), at(first), "private ", kind);
    add(at(i) - 1, at(i), "", nullptr);
  }
}

bool FilePort::chainMayStart(size_t i) const {
  if (i == 0) {
    return true;
  }
  switch (this->toks[i - 1].kind) {
    case Tok::LParen:
    case Tok::LBracket:
    case Tok::Comma:
    case Tok::Semi:
    case Tok::Colon:
    case Tok::Question:
    case Tok::Assign:
    case Tok::PlusAssign:
    case Tok::Arrow:
    case Tok::KwReturn:
    case Tok::KwCase:
    case Tok::AmpAmp:
    case Tok::PipePipe:
    case Tok::QuestionQuestion:
    case Tok::EqEq:
    case Tok::BangEq:
    case Tok::EqEqEq:
    case Tok::BangEqEq:
    case Tok::TemplateHead:
    case Tok::TemplateMiddle:
      return true;
    case Tok::Identifier:
      return this->toks[i - 1].text == "throw";
    default:
      return false;
  }
}

bool FilePort::chainEnds(size_t i) const {
  if (i >= this->toks.size() || this->toks[i].newlineBefore) {
    return true;
  }
  switch (this->toks[i].kind) {
    case Tok::RParen:
    case Tok::RBracket:
    case Tok::RBrace:
    case Tok::Comma:
    case Tok::Semi:
    case Tok::Colon:
    case Tok::Question:
    case Tok::QuestionQuestion:
    case Tok::AmpAmp:
    case Tok::PipePipe:
    case Tok::EqEq:
    case Tok::BangEq:
    case Tok::EqEqEq:
    case Tok::BangEqEq:
    case Tok::TemplateMiddle:
    case Tok::TemplateTail:
    case Tok::KwAs:
      return true;
    default:
      return false;
  }
}

void FilePort::concatenations() {
  for (size_t i = 0; i < this->toks.size(); ++i) {
    if (!chainMayStart(i)) {
      continue;
    }
    size_t end = operand(i);
    if (!end) {
      continue;
    }
    std::vector<std::pair<size_t, size_t>> parts{{i, end}};
    std::vector<size_t> ops;
    bool ok = true;
    while (is(end, Tok::Plus) || is(end, Tok::Minus)) {
      size_t next = operand(end + 1);
      if (!next) {
        ok = false;
        break;
      }
      ops.push_back(end);
      parts.push_back({end + 1, next});
      end = next;
    }
    if (!ok || ops.empty() || !chainEnds(end)) {
      continue;
    }
    auto literal = [&](const std::pair<size_t, size_t> &p) {
      return (p.second == p.first + 1 &&
              (is(p.first, Tok::StringLiteral) || is(p.first, Tok::TemplateString))) ||
             (is(p.first, Tok::TemplateHead) && this->close[p.first] + 1 == p.second);
    };
    size_t first = 0;
    while (first < parts.size() && !literal(parts[first])) {
      ++first;
    }
    bool allLiteral = llvm::all_of(parts, literal);
    bool plusAfter = true;
    for (size_t k = first == 0 ? 0 : first - 1; k < ops.size(); ++k) {
      plusAfter &= is(ops[k], Tok::Plus);
    }
    bool quiet = true;
    for (size_t k = i; k + 1 < end; ++k) {
      if (!llvm::StringRef(text_).slice(endOf(k), at(k + 1)).trim().empty()) {
        quiet = false;
      }
    }
    if (first == parts.size() || allLiteral || !plusAfter || !quiet) {
      i = end - 1;
      continue;
    }
    auto source = [&](size_t from, size_t to) {
      return text_.substr(at(from), endOf(to - 1) - at(from));
    };
    std::string out = "`";
    if (first > 0) {
      out += "${" + source(parts[0].first, parts[first - 1].second) + "}";
    }
    for (size_t k = first; k < parts.size(); ++k) {
      auto [from, to] = parts[k];
      if (is(from, Tok::StringLiteral) && to == from + 1) {
        out += FilePort::templateText(this->toks[from].text);
      } else if (literal(parts[k])) {
        out += llvm::StringRef(source(from, to)).drop_front().drop_back().str();
      } else {
        out += "${" + source(from, to) + "}";
      }
    }
    out += "`";
    add(at(i), endOf(end - 1), out, "'+' with a string became a template literal");
    i = end - 1;
  }
}

void FilePort::manual() {
  std::vector<std::pair<size_t, size_t>> lists;
  std::map<std::string, unsigned> functions;
  for (size_t i = 0; i < this->toks.size(); ++i) {
    const Token &t = this->toks[i];
    if ((isName(i, "import") || isName(i, "export")) && !afterDot(i)) {
      size_t j = i + 1;
      if (isName(j, "type")) {
        ++j;
      }
      if (isName(j, "import") || isName(j, "export")) {
        continue;
      }
      if (is(j, Tok::Identifier) && is(j + 1, Tok::Comma)) {
        j += 2;
      }
      if (is(j, Tok::LBrace) && this->close[j] != std::string::npos) {
        lists.push_back({j, this->close[j]});
      }
    }
    bool inList = llvm::any_of(lists, [&](auto &l) { return i > l.first && i < l.second; });
    if (afterDot(i) && !isName(i, "prototype") && !isName(i, "__proto__")) {
      continue;
    }

    if (t.is(Tok::KwAs) && !inList && i > 0 && FilePort::isOperandEnd(this->toks[i - 1])) {
      if (is(i + 1, Tok::KwConst)) {
        note(i, "'as const' is not supported: declare the type of the value instead, e.g. a "
                "union of literal types");
      } else if (isName(i + 1, "unknown") && is(i + 2, Tok::KwAs)) {
        note(i, "'as unknown as' is not supported: tz has no casts; check the value (typeof, "
                "instanceof, 'in') to narrow it, or give it the right type where it is made");
      } else {
        note(i, "'as' is not supported: remove it; if the value's type is wider than written, "
                "narrow it with a check (typeof, instanceof, 'in', '!= undefined')");
      }
      continue;
    }
    if (isName(i, "throw") && !inList) {
      note(i, "'throw' is not supported (docs/errors.md): return the error, 'return err(...)', "
              "and give the function a 'Result'");
      continue;
    }
    if (isName(i, "try") && is(i + 1, Tok::LBrace)) {
      note(i, "'try' is not supported (docs/errors.md): a function that can fail gives a "
              "'Result', checked with 'if'");
      continue;
    }
    if (t.is(Tok::KwNew) && isName(i + 1, "Function") && is(i + 2, Tok::LParen)) {
      note(i, "code made at run time ('new Function', 'eval') is not supported");
      continue;
    }
    if (t.is(Tok::KwFunction) && is(i + 1, Tok::Identifier)) {
      if (++functions[this->toks[i + 1].text.str()] == 2) {
        note(i, "several signatures with one body ('" + this->toks[i + 1].text.str() +
                    "') are not supported for plain functions (docs/overloads.md): keep one "
                    "signature with unions, or give each its own function");
      }
      continue;
    }
    if (!t.is(Tok::Identifier)) {
      continue;
    }
    llvm::StringRef name = t.text;
    if (name == "satisfies" && i > 0 && FilePort::isOperandEnd(this->toks[i - 1])) {
      note(i, "'satisfies' is not supported: declare the type of the variable instead, or "
              "remove it");
    } else if (name == "asserts" && i > 0 && is(i - 1, Tok::Colon) && is(i + 1, Tok::Identifier) &&
               !t.newlineBefore) {
      note(i, "'asserts' is not supported (docs/narrowing.md): there are no exceptions, so a "
              "function cannot stop its caller; write '" +
                  this->toks[i + 1].text.str() +
                  " is T' and return whether it holds, then check it at the call: 'if "
                  "(!isT(x)) return err(...)'");
    } else if ((name == "get" || name == "set") &&
               (is(i + 1, Tok::Identifier) || is(i + 1, Tok::StringLiteral) ||
                is(i + 1, Tok::LBracket)) &&
               is(i + 2, Tok::LParen) &&
               (i == 0 || t.newlineBefore || is(i - 1, Tok::LBrace) || is(i - 1, Tok::Semi) ||
                is(i - 1, Tok::RBrace) || is(i - 1, Tok::Comma) || isName(i - 1, "static") ||
                isName(i - 1, "public") || isName(i - 1, "private") || isName(i - 1, "protected") ||
                isName(i - 1, "override"))) {
      note(i, "getters and setters are not supported (docs/objects.md): reading a field never "
              "runs code; make '" +
                  name.str() + " " + this->toks[i + 1].text.str() +
                  "()' a method, or keep the value in a field");
    } else if (name == "prototype" || name == "__proto__") {
      note(i, "prototypes are not supported (docs/objects.md): declare the methods in a class");
    } else if (name == "Object" && is(i + 1, Tok::Dot) &&
               (isName(i + 2, "setPrototypeOf") || isName(i + 2, "getPrototypeOf") ||
                isName(i + 2, "create") || isName(i + 2, "defineProperty") ||
                isName(i + 2, "defineProperties") || isName(i + 2, "freeze"))) {
      note(i, "'Object." + this->toks[i + 2].text.str() +
                  "' is not supported: objects have the fields of their type, and no prototypes "
                  "(docs/objects.md)");
    } else if (name == "arguments" && !is(i + 1, Tok::Colon)) {
      note(i, "'arguments' is not supported: declare a rest parameter, e.g. '...args: "
              "string[]'");
    } else if (name == "require" && is(i + 1, Tok::LParen)) {
      note(i, "'require' is not supported: import by name, e.g. 'import { x } from \"./x\"'");
    } else if (name == "eval" && is(i + 1, Tok::LParen)) {
      note(i, "code made at run time ('eval', 'new Function') is not supported");
    } else if ((name == "namespace" || name == "module") && is(i + 1, Tok::Identifier) &&
               (is(i + 2, Tok::LBrace) || is(i + 2, Tok::Dot))) {
      note(i, "'" + name.str() +
                  "' is not supported: move what it declares into a file of its own and import "
                  "it by name (docs/modules.md)");
    } else if (name == "declare" && (isName(i + 1, "global") || isName(i + 1, "module"))) {
      note(i, "'declare " + this->toks[i + 1].text.str() + "' is not supported (docs/modules.md)");
    } else if (name == "bigint" || (name == "BigInt" && is(i + 1, Tok::LParen))) {
      note(i, "bigint is not supported (docs/decimal.md): use long, or decimal for exact big "
              "numbers");
    } else if ((name == "Uppercase" || name == "Lowercase" || name == "Capitalize" ||
                name == "Uncapitalize") &&
               is(i + 1, Tok::Less)) {
      note(i, "'" + name.str() + "<...>' is not supported: write 'string'");
    } else if (name == "import" && is(i + 1, Tok::StringLiteral)) {
      note(i, "an import for side effects is not supported: import what the file uses by name");
    } else if (name == "import" && is(i + 1, Tok::LParen)) {
      note(i, "'import()' is not supported: import by name at the top of the file");
    } else if (name == "import" && is(i + 1, Tok::Identifier) && is(i + 2, Tok::Assign)) {
      note(i, "'import x = require()' is not supported: import by name");
    } else if (name == "export" && is(i + 1, Tok::Assign)) {
      note(i, "'export =' is not supported: export the names with 'export'");
    } else if (name == "export" && is(i + 1, Tok::Star) && is(i + 2, Tok::KwAs)) {
      note(i, "'export * as' is not supported: export the names, or 'export *'");
    } else if (name == "from" && is(i + 1, Tok::StringLiteral)) {
      std::string spec = Scanner::stringValue(this->toks[i + 1]);
      llvm::StringRef s = spec;
      std::string package, sub;
      if (s.starts_with("npm:")) {
        note(i + 1, "npm packages are not supported (docs/packages.md): take the package from "
                    "JSR or git, adapted for tz, or do without it");
      } else if (!s.starts_with(".") && !s.contains(':') &&
                 !BuiltinModulePaths::isBuiltinModule(s) && s != "neverthrow" &&
                 Specifier::splitPackageSpecifier(s, package, sub) &&
                 !this->ctx.jsrDependencies.count(package) &&
                 this->notedPackages.insert(package).second) {
        note(i + 1, "'" + package +
                        "' is another package: add it to package.json once it is adapted for tz "
                        "(docs/packages.md), or do without it");
      }
    }
  }
  for (size_t i = 1; i < this->toks.size(); ++i) {
    if ((is(i, Tok::TemplateHead) || is(i, Tok::TemplateString)) &&
        (is(i - 1, Tok::Pipe) || (is(i - 1, Tok::Assign) && i >= 3 && isName(i - 3, "type")))) {
      note(i, "template literal types are not supported: write 'string'");
    }
  }
}

void FilePort::fieldValues() {
  for (size_t i = 0; i < this->toks.size(); ++i) {
    if (!isName(i, "class") || afterDot(i)) {
      continue;
    }
    ClassShape shape = this->classShape(i);
    if (shape.body != std::string::npos) {
      this->moveFieldValues(shape);
    }
  }
}

bool FilePort::opens(size_t i) const {
  Tok kind = this->toks[i].kind;
  bool opening = kind == Tok::LParen || kind == Tok::LBracket || kind == Tok::LBrace ||
                 kind == Tok::TemplateHead;

  return opening && this->close[i] != std::string::npos;
}

size_t FilePort::angleEnd(size_t less) const {
  int angles = 0;
  for (size_t j = less; j < this->toks.size(); ++j) {
    Tok kind = this->toks[j].kind;
    if (kind == Tok::Semi) {
      return j;
    }
    if (kind == Tok::Less) {
      angles += 1;
    } else if (kind == Tok::Greater) {
      angles -= 1;
    } else if (kind == Tok::Shr) {
      angles -= 2;
    } else if (kind == Tok::UShr) {
      angles -= 3;
    } else if (this->opens(j)) {
      j = this->close[j];
    }
    if (angles <= 0) {
      return j + 1;
    }
  }

  return this->toks.size();
}

ClassShape FilePort::classShape(size_t i) const {
  ClassShape shape;
  size_t j = i + 1;
  while (j < this->toks.size()) {
    if (is(j, Tok::Less)) {
      j = this->angleEnd(j);
      continue;
    }
    if (is(j, Tok::LBrace)) {
      shape.body = this->opens(j) ? j : std::string::npos;
      return shape;
    }
    if (is(j, Tok::LParen) && this->opens(j)) {
      j = this->close[j] + 1;
      continue;
    }
    bool header = is(j, Tok::Identifier) || is(j, Tok::Dot) || is(j, Tok::Comma);
    if (!header) {
      return shape;
    }
    shape.extends = shape.extends || isName(j, "extends");
    j += 1;
  }

  return shape;
}

bool FilePort::isMemberModifier(size_t i) const {
  static const llvm::StringLiteral modifiers[] = {
      "public", "private", "protected", "readonly", "static", "override",
      "declare", "abstract", "accessor", "async", "get", "set"};
  if (!is(i, Tok::Identifier) || !llvm::is_contained(modifiers, this->toks[i].text)) {
    return false;
  }
  size_t next = i + 1;
  if (next >= this->toks.size() || this->toks[next].newlineBefore) {
    return false;
  }

  return is(next, Tok::Identifier) || is(next, Tok::StringLiteral) || is(next, Tok::LBracket) ||
         is(next, Tok::Star) || is(next, Tok::IntLiteral);
}

bool FilePort::continuesType(size_t i) const {
  Tok next = this->toks[i].kind;
  Tok previous = this->toks[i - 1].kind;

  return next == Tok::Pipe || next == Tok::Amp || previous == Tok::Pipe ||
         previous == Tok::Amp || previous == Tok::Arrow || previous == Tok::Colon ||
         previous == Tok::Comma;
}

bool FilePort::continuesValue(size_t i) const {
  switch (this->toks[i].kind) {
    case Tok::Dot:
    case Tok::QuestionDot:
    case Tok::Question:
    case Tok::Colon:
    case Tok::Arrow:
    case Tok::Comma:
    case Tok::LParen:
    case Tok::LBracket:
    case Tok::TemplateString:
    case Tok::TemplateHead:
    case Tok::Plus:
    case Tok::Minus:
    case Tok::Star:
    case Tok::StarStar:
    case Tok::Slash:
    case Tok::Percent:
    case Tok::Amp:
    case Tok::Pipe:
    case Tok::Caret:
    case Tok::AmpAmp:
    case Tok::PipePipe:
    case Tok::QuestionQuestion:
    case Tok::Shl:
    case Tok::Shr:
    case Tok::UShr:
    case Tok::Less:
    case Tok::LessEq:
    case Tok::Greater:
    case Tok::GreaterEq:
    case Tok::EqEq:
    case Tok::EqEqEq:
    case Tok::BangEq:
    case Tok::BangEqEq:
    case Tok::KwAs:
    case Tok::KwIn:
      return true;
    default:
      return false;
  }
}

size_t FilePort::typeEnd(size_t from, size_t end) const {
  size_t j = from;
  while (j < end) {
    bool lineEnds = j > from && this->toks[j].newlineBefore && !this->continuesType(j);
    if (is(j, Tok::Assign) || is(j, Tok::Semi) || lineEnds) {
      return j;
    }
    if (is(j, Tok::Less)) {
      j = this->angleEnd(j);
      continue;
    }
    if (this->opens(j)) {
      j = this->close[j];
    }
    j += 1;
  }

  return end;
}

size_t FilePort::valueEnd(size_t from, size_t end) const {
  size_t j = from;
  while (j < end) {
    const Token &previous = this->toks[j - 1];
    bool operandEnds = FilePort::isOperandEnd(previous) || previous.is(Tok::PlusPlus) ||
                       previous.is(Tok::MinusMinus);
    bool lineEnds = j > from && this->toks[j].newlineBefore && operandEnds &&
                    !this->continuesValue(j);
    if (is(j, Tok::Semi) || lineEnds) {
      return j;
    }
    if (this->opens(j)) {
      j = this->close[j];
    }
    j += 1;
  }

  return end;
}

bool FilePort::isFunctionValue(size_t from, size_t to) const {
  if (is(from, Tok::KwFunction) || isName(from, "async")) {
    return true;
  }
  if (is(from, Tok::Identifier) && from + 1 < to && is(from + 1, Tok::Arrow)) {
    return true;
  }
  if (!is(from, Tok::LParen) && !is(from, Tok::Less)) {
    return false;
  }
  for (size_t j = from; j < to; ++j) {
    if (is(j, Tok::Arrow)) {
      return true;
    }
    if (this->opens(j)) {
      j = this->close[j];
    }
  }

  return false;
}

std::string FilePort::newType(size_t from, size_t to) const {
  if (!is(from, Tok::KwNew) || !is(from + 1, Tok::Identifier)) {
    return "";
  }
  size_t j = from + 2;
  while (is(j, Tok::Dot) && is(j + 1, Tok::Identifier)) {
    j += 2;
  }
  bool generic = is(j, Tok::Less);
  if (generic) {
    j = this->angleEnd(j);
  }
  if (!is(j, Tok::LParen) || !this->opens(j) || this->close[j] + 1 != to) {
    return "";
  }
  static const llvm::StringLiteral needArguments[] = {"Map",     "Set",     "Array",
                                                      "WeakMap", "WeakSet", "Promise"};
  if (!generic && llvm::is_contained(needArguments, this->toks[from + 1].text)) {
    return "";
  }
  size_t begin = at(from + 1);

  return text_.substr(begin, endOf(j - 1) - begin);
}

std::string FilePort::valueType(size_t from, size_t to) const {
  bool negative = is(from, Tok::Minus);
  size_t first = negative ? from + 1 : from;
  if (to != first + 1) {
    return this->newType(from, to);
  }
  switch (this->toks[first].kind) {
    case Tok::IntLiteral:
      return "int";
    case Tok::FloatLiteral:
      return "double";
    case Tok::StringLiteral:
    case Tok::TemplateString:
      return negative ? "" : "string";
    case Tok::KwTrue:
    case Tok::KwFalse:
      return negative ? "" : "boolean";
    default:
      return "";
  }
}

llvm::StringSet<> FilePort::parameterNames(size_t paren) const {
  llvm::StringSet<> names;
  bool expectName = true;
  for (size_t j = paren + 1; j < this->close[paren]; ++j) {
    if (is(j, Tok::Comma)) {
      expectName = true;
      continue;
    }
    if (is(j, Tok::At)) {
      while (is(j + 1, Tok::Identifier) || is(j + 1, Tok::Dot)) {
        j += 1;
      }
      if (is(j + 1, Tok::LParen) && this->opens(j + 1)) {
        j = this->close[j + 1];
      }
      continue;
    }
    bool pattern = is(j, Tok::LBrace) || is(j, Tok::LBracket);
    if (expectName && pattern && this->opens(j)) {
      for (size_t m = j + 1; m < this->close[j]; ++m) {
        if (is(m, Tok::Identifier) && !afterDot(m)) {
          names.insert(this->toks[m].text);
        }
      }
      expectName = false;
    }
    if (this->opens(j)) {
      j = this->close[j];
      continue;
    }
    if (expectName && is(j, Tok::Identifier) && !this->isMemberModifier(j)) {
      names.insert(this->toks[j].text);
      expectName = false;
    }
  }

  return names;
}

bool FilePort::usesAny(const FieldValue &field, const llvm::StringSet<> &names) const {
  for (size_t j = field.valueBegin; j < field.valueEnd; ++j) {
    if (is(j, Tok::Identifier) && !afterDot(j) && names.contains(this->toks[j].text)) {
      return true;
    }
  }

  return false;
}

size_t FilePort::classMember(size_t k, size_t end, ClassMembers &members) const {
  size_t j = k;
  if (is(j, Tok::Semi)) {
    return j + 1;
  }
  while (is(j, Tok::At)) {
    j += 1;
    while (is(j, Tok::Identifier) || is(j, Tok::Dot)) {
      j += 1;
    }
    if (is(j, Tok::LParen) && this->opens(j)) {
      j = this->close[j] + 1;
    }
  }
  bool shared = false;
  while (this->isMemberModifier(j)) {
    shared = shared || isName(j, "static");
    j += 1;
  }
  if (is(j, Tok::Star)) {
    j += 1;
  }
  size_t name = j;
  if (is(j, Tok::LBracket) && this->opens(j)) {
    j = this->close[j];
  }
  if (j >= end) {
    return end;
  }
  j += 1;
  if (is(j, Tok::Question) || is(j, Tok::Bang)) {
    j += 1;
  }
  if (is(j, Tok::Less)) {
    j = this->angleEnd(j);
  }
  if (is(j, Tok::LBrace) && this->opens(j)) {
    return this->close[j] + 1;
  }
  if (is(j, Tok::LParen) && this->opens(j)) {
    if (isName(name, "constructor")) {
      members.constructors.push_back(j);
    }
    size_t m = this->close[j] + 1;
    while (m < end) {
      bool typeLiteral = is(m - 1, Tok::Colon) || is(m - 1, Tok::Pipe) ||
                         is(m - 1, Tok::Amp) || is(m - 1, Tok::Comma) || is(m - 1, Tok::Arrow);
      if (is(m, Tok::LBrace) && this->opens(m) && !typeLiteral) {
        return this->close[m] + 1;
      }
      if (is(m, Tok::Semi)) {
        return m + 1;
      }
      if (this->toks[m].newlineBefore && !typeLiteral && !this->continuesType(m)) {
        return m;
      }
      if (this->opens(m)) {
        m = this->close[m];
      }
      m += 1;
    }
    return end;
  }
  FieldValue field;
  field.name = name;
  field.declarationEnd = j - 1;
  if (is(j, Tok::Colon)) {
    field.typed = true;
    j = this->typeEnd(j + 1, end);
    field.declarationEnd = j - 1;
  }
  bool known = j >= end || is(j, Tok::Assign) || is(j, Tok::Semi) || this->toks[j].newlineBefore;
  if (!known) {
    return std::max(j, k + 1);
  }
  members.lastField = j - 1;
  if (is(j, Tok::Assign)) {
    size_t value = j + 1;
    size_t after = this->valueEnd(value, end);
    field.valueBegin = value;
    field.valueEnd = after;
    field.semicolon = is(after, Tok::Semi);
    members.lastField = after - 1;
    bool function = this->isFunctionValue(value, after);
    if (!shared && !function && after > value) {
      members.values.push_back(field);
    }
    j = after;
  }
  if (is(j, Tok::Semi)) {
    return j + 1;
  }

  return std::max(j, k + 1);
}

std::string FilePort::lineBreak() const {
  return text_.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

std::string FilePort::lineIndent(size_t offset) const {
  size_t newline = offset == 0 ? std::string::npos : text_.rfind('\n', offset - 1);
  size_t start = newline == std::string::npos ? 0 : newline + 1;
  size_t stop = start;
  while (stop < text_.size() && (text_[stop] == ' ' || text_[stop] == '\t')) {
    stop += 1;
  }

  return text_.substr(start, stop - start);
}

size_t FilePort::lineAfter(size_t offset) const {
  size_t newline = text_.find('\n', offset);

  return newline == std::string::npos ? std::string::npos : newline + 1;
}

std::string FilePort::fieldStatements(const std::vector<FieldValue> &fields,
                                      const std::string &indent) const {
  std::string out;
  for (const FieldValue &field : fields) {
    size_t begin = at(field.valueBegin);
    std::string value = text_.substr(begin, endOf(field.valueEnd - 1) - begin);
    std::string fieldIndent = this->lineIndent(at(field.name));
    if (indent.size() > fieldIndent.size()) {
      std::string shift(indent.size() - fieldIndent.size(), ' ');
      std::string shifted;
      for (char c : value) {
        shifted += c;
        if (c == '\n') {
          shifted += shift;
        }
      }
      value = shifted;
    }
    std::string name = this->toks[field.name].text.str();
    std::string end = field.semicolon ? ";" : "";
    out += indent + "this." + name + " = " + value + end + this->lineBreak();
  }

  return out;
}

std::optional<Edit> FilePort::constructorEdit(size_t paren, bool extends,
                                              const std::vector<FieldValue> &fields) const {
  size_t body = this->close[paren] + 1;
  if (!is(body, Tok::LBrace) || !this->opens(body)) {
    return std::nullopt;
  }
  size_t bodyEnd = this->close[body];
  llvm::StringRef text(text_);
  if (extends) {
    for (size_t m = body + 1; m < bodyEnd; ++m) {
      if (isName(m, "super") && is(m + 1, Tok::LParen) && this->opens(m + 1)) {
        size_t last = is(this->close[m + 1] + 1, Tok::Semi) ? this->close[m + 1] + 1
                                                            : this->close[m + 1];
        size_t line = this->lineAfter(endOf(last));
        bool alone = line != std::string::npos && line <= at(bodyEnd) &&
                     text.slice(endOf(last), line).trim().empty();
        if (!alone) {
          return std::nullopt;
        }
        std::string statements = this->fieldStatements(fields, this->lineIndent(at(m)));
        return Edit{line, line, statements, nullptr, ""};
      }
      if (this->opens(m)) {
        m = this->close[m];
      }
    }
    return std::nullopt;
  }
  std::string constructorIndent = this->lineIndent(at(paren));
  if (body + 1 == bodyEnd && !this->toks[bodyEnd].newlineBefore) {
    std::string inner = this->fieldStatements(fields, constructorIndent + "  ");
    std::string replaced = "{" + this->lineBreak() + inner + constructorIndent + "}";
    return Edit{at(body), endOf(bodyEnd), replaced, nullptr, ""};
  }
  size_t line = this->lineAfter(endOf(body));
  bool opensLine = line != std::string::npos && line <= at(bodyEnd) &&
                   text.slice(endOf(body), line).trim().empty();
  if (!opensLine) {
    return std::nullopt;
  }
  bool statementBelow = body + 1 < bodyEnd && this->toks[body + 1].newlineBefore;
  std::string indent =
      statementBelow ? this->lineIndent(at(body + 1)) : constructorIndent + "  ";
  if (indent.size() <= constructorIndent.size()) {
    indent = constructorIndent + "  ";
  }

  return Edit{line, line, this->fieldStatements(fields, indent), nullptr, ""};
}

std::optional<Edit> FilePort::newConstructor(size_t lastField,
                                             const std::vector<FieldValue> &fields) const {
  size_t line = this->lineAfter(endOf(lastField));
  llvm::StringRef text(text_);
  if (line == std::string::npos) {
    return std::nullopt;
  }
  llvm::StringRef rest = text.slice(endOf(lastField), line).trim();
  if (!rest.empty() && rest != ";") {
    return std::nullopt;
  }
  std::string newline = this->lineBreak();
  std::string indent = this->lineIndent(at(fields.front().name));
  llvm::StringRef next = text.slice(line, text.find('\n', line)).trim();
  std::string tail = next.empty() || next == "}" ? "" : newline;
  std::string statements = this->fieldStatements(fields, indent + "  ");
  std::string made = newline + indent + "constructor() {" + newline + statements + indent + "}" +
                     newline + tail;

  return Edit{line, line, made, nullptr, ""};
}

void FilePort::moveFieldValues(const ClassShape &shape) {
  size_t end = this->close[shape.body];
  ClassMembers members;
  size_t k = shape.body + 1;
  while (k < end) {
    k = this->classMember(k, end, members);
  }
  std::vector<FieldValue> moved;
  bool ownConstructor = !members.constructors.empty();
  for (const FieldValue &field : members.values) {
    llvm::StringRef name = this->toks[field.name].text;
    bool hidden = false;
    for (size_t paren : members.constructors) {
      hidden = hidden || this->usesAny(field, this->parameterNames(paren));
    }
    std::string type = this->valueType(field.valueBegin, field.valueEnd);
    bool typeKnown = field.typed || !type.empty();
    bool literal = !type.empty() && !is(field.valueBegin, Tok::KwNew);
    if (!is(field.name, Tok::Identifier)) {
      continue;
    }
    if (!ownConstructor && literal) {
      if (!field.typed) {
        add(endOf(field.declarationEnd), endOf(field.declarationEnd), ": " + type,
            "'x = 0' became 'x: int = 0'");
      }
      continue;
    }
    if (hidden) {
      note(field.name, "a field gets its value in the constructor (docs/objects.md), and the "
                       "value of '" + name.str() +
                           "' uses a name a parameter of the constructor hides: move it there "
                           "by hand");
      continue;
    }
    if (!typeKnown) {
      note(field.name, "a field gets its value in the constructor (docs/objects.md), and only "
                       "a literal shows the type of '" + name.str() + "': write '" +
                           name.str() + ": T' and set it there, 'this." + name.str() +
                           " = ...'");
      continue;
    }
    moved.push_back(field);
  }
  if (moved.empty()) {
    return;
  }
  std::vector<Edit> sets;
  for (size_t paren : members.constructors) {
    std::optional<Edit> set = this->constructorEdit(paren, shape.extends, moved);
    if (set.has_value()) {
      sets.push_back(*set);
    }
  }
  bool placed = members.constructors.empty() ? !shape.extends
                                             : sets.size() == members.constructors.size();
  if (members.constructors.empty() && placed) {
    std::optional<Edit> made = this->newConstructor(members.lastField, moved);
    placed = made.has_value();
    if (placed) {
      sets.push_back(*made);
    }
  }
  if (!placed) {
    for (const FieldValue &field : moved) {
      std::string name = this->toks[field.name].text.str();
      note(field.name, "a field gets its value in the constructor (docs/objects.md): set '" +
                           name + "' there by hand, 'this." + name + " = ...'");
    }
    return;
  }
  for (const FieldValue &field : moved) {
    std::string type =
        field.typed ? "" : ": " + this->valueType(field.valueBegin, field.valueEnd);
    add(endOf(field.declarationEnd), endOf(field.valueEnd - 1), type,
        "field values moved into the constructor");
  }
  this->edits.insert(this->edits.end(), sets.begin(), sets.end());
}

}
