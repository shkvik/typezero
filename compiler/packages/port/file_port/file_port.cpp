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

namespace {

bool isOperandEnd(const Token &t) {
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

std::string templateText(llvm::StringRef literal) {
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

}

void FilePort::retokenize() {
  toks_ = tokenize(text_);
  close_.assign(toks_.size(), std::string::npos);
  std::vector<size_t> open;
  for (size_t i = 0; i < toks_.size(); ++i) {
    Tok k = toks_[i].kind;
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
    if (!open.empty() && toks_[open.back()].kind == want) {
      close_[open.back()] = i;
      open.pop_back();
    }
  }
}

bool FilePort::apply() {
  if (edits_.empty()) {
    return false;
  }
  std::stable_sort(edits_.begin(), edits_.end(),
                   [](const Edit &a, const Edit &b) { return a.begin < b.begin; });
  std::vector<const Edit *> taken;
  size_t last = 0;
  for (const Edit &edit : edits_) {
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
      ++counts_[edit->kind];
    }
  }
  out.append(text_, from, std::string::npos);
  text_ = std::move(out);
  edits_.clear();

  return true;
}

void FilePort::run() {
  retokenize();
  modules();
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
  manual();
  std::stable_sort(notes_.begin(), notes_.end(),
                   [](const Note &a, const Note &b) { return a.offset < b.offset; });
}

void FilePort::modules() {
  for (size_t i = 0; i < toks_.size(); ++i) {
    if (afterDot(i)) {
      continue;
    }
    if (isName(i, "export")) {
      if (is(i + 1, Tok::KwDefault)) {
        exportDefault(i);
      } else if (isName(i + 1, "type") && is(i + 2, Tok::LBrace)) {
        add(at(i + 1), at(i + 2), "", "'import type' and 'export type' lost 'type'");
        exportList(i + 2, close_[i + 2]);
      } else if (isName(i + 1, "type") && is(i + 2, Tok::Star)) {
        add(at(i + 1), at(i + 2), "", "'import type' and 'export type' lost 'type'");
      } else if (is(i + 1, Tok::LBrace)) {
        exportList(i + 1, close_[i + 1]);
      }
    } else if (isName(i, "import")) {
      importDecl(i);
    }
  }
  for (size_t i = 1; i < toks_.size(); ++i) {
    if (!is(i, Tok::StringLiteral) ||
        !(isName(i - 1, "from") || (isName(i - 1, "import") && !afterDot(i - 1)))) {
      continue;
    }
    std::string spec = stringValue(toks_[i]);
    llvm::StringRef ref(spec);
    llvm::StringRef ext = path::extension(ref, path::Style::posix);
    if (!(ref.starts_with("./") || ref.starts_with("../")) || ref.ends_with(".d.ts") ||
        (ext != ".ts" && ext != ".js")) {
      continue;
    }
    char quote = toks_[i].text.front();
    add(at(i), endOf(i), quote + ref.drop_back(3).str() + ".tz" + quote,
        "imports of .ts and .js files name the .tz files of the copy");
  }
}

void FilePort::exportDefault(size_t i) {
  const char *kind = "'export default' became a named export";
  size_t def = i + 1, k = i + 2;
  if (k >= toks_.size()) {
    return;
  }
  llvm::SmallString<256> real;
  fs::real_path(file_, real);
  auto known = ctx_.defaultNames.find(std::string(real));
  std::string name = known != ctx_.defaultNames.end() ? known->second : "";
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
    bool named = is(last + 1, Tok::Identifier) && toks_[last + 1].text != "extends" &&
                 toks_[last + 1].text != "implements";
    if (named) {
      add(at(def), at(k), "", kind);
    } else {
      std::string words = text_.substr(at(k), endOf(last) - at(k));
      if (last + 1 < toks_.size()) {
        add(at(def), at(last + 1), words + " " + name + (is(last + 1, Tok::LParen) ? "" : " "),
            kind);
      } else {
        add(at(def), endOf(last), words + " " + name, kind);
      }
    }
    return;
  }
  if (is(k, Tok::Identifier) &&
      (k + 1 >= toks_.size() || is(k + 1, Tok::Semi) || toks_[k + 1].newlineBefore)) {
    add(at(i), endOf(k), "export { " + toks_[k].text.str() + " }", kind);
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
    std::string spec = stringValue(toks_[close + 2]);
    std::string file = resolveRelative(file_, spec, Sources::TypeScript);
    llvm::SmallString<256> real;
    if (!file.empty() && !fs::real_path(file, real)) {
      if (auto it = ctx_.defaultNames.find(std::string(real)); it != ctx_.defaultNames.end()) {
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
  for (size_t j = 0; j < toks_.size(); ++j) {
    if ((j < skipBegin || j >= skipEnd) && is(j, Tok::Identifier) && !afterDot(j)) {
      own.insert(toks_[j].text);
    }
  }
  std::vector<Edit> uses;
  std::set<std::string> seen;
  for (size_t j = 0; j < toks_.size(); ++j) {
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
    std::string member = toks_[j + 2].text.str();
    std::string local = own.contains(member) ? name + capitalize(member) : member;
    uses.push_back({at(j), endOf(j + 2), local, nullptr, ""});
    if (seen.insert(member).second) {
      items.push_back(local == member ? member : member + " as " + local);
    }
  }
  for (Edit &use : uses) {
    edits_.push_back(std::move(use));
  }

  return true;
}

void FilePort::importDecl(size_t i) {
  if (is(i + 1, Tok::StringLiteral) || is(i + 1, Tok::LParen)) {
    return;
  }
  size_t from = std::string::npos;
  for (size_t j = i + 1; j < toks_.size() && j < i + 2000; ++j) {
    if (is(j, Tok::LBrace) && close_[j] != std::string::npos) {
      j = close_[j];
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
  std::string spec = stringValue(toks_[from + 1]);
  llvm::StringRef specRef(spec);

  std::string package, source, sub;
  if (splitJsrImport(specRef, package, source, sub)) {
    char quote = toks_[from + 1].text.front();
    add(at(from + 1), endOf(from + 1), std::string(1, quote) + package + sub + quote,
        "'jsr:' imports became package names in tz.json");
  }

  std::string defaultName, ns;
  if (is(c, Tok::Identifier) && (is(c + 1, Tok::Comma) || c + 1 == from)) {
    defaultName = toks_[c].text.str();
    c += is(c + 1, Tok::Comma) ? 2 : 1;
  }
  if (is(c, Tok::Star) && is(c + 1, Tok::KwAs) && is(c + 2, Tok::Identifier)) {
    ns = toks_[c + 2].text.str();
    c += 3;
  }
  std::vector<std::string> items;
  if (is(c, Tok::LBrace) && close_[c] != std::string::npos) {
    bool rewrite = !defaultName.empty() || !ns.empty();
    std::string item;
    for (size_t j = c + 1; j < close_[c]; ++j) {
      if (isName(j, "type") && is(j + 1, Tok::Identifier)) {
        if (!rewrite) {
          add(at(j), at(j + 1), "", typeKind);
        } else {
          ++counts_[typeKind];
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
      item += (item.empty() ? "" : " ") + toks_[j].text.str();
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
      std::string target = resolveRelative(file_, spec, Sources::TypeScript);
      llvm::SmallString<256> real;
      if (!target.empty() && !fs::real_path(target, real)) {
        if (auto it = ctx_.defaultNames.find(std::string(real)); it != ctx_.defaultNames.end()) {
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
  for (size_t i = 0; i < toks_.size(); ++i) {
    const Token &t = toks_[i];
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
      if (is(i + 1, Tok::IntLiteral) && toks_[i + 1].text == "0") {
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
  size_t n = toks_.size();
  while (i < n && (is(i, Tok::Bang) || is(i, Tok::Tilde) || is(i, Tok::Minus) || is(i, Tok::Plus) ||
                   is(i, Tok::KwTypeof) || is(i, Tok::KwVoid) || is(i, Tok::KwDelete) ||
                   isName(i, "await"))) {
    ++i;
  }
  if (i >= n) {
    return 0;
  }
  auto closed = [&](size_t j) { return close_[j] == std::string::npos ? 0 : close_[j] + 1; };
  switch (toks_[i].kind) {
  case Tok::Identifier: {
    llvm::StringRef name = toks_[i].text;
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
    const Token &t = toks_[i];
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
  bool declared =
      !afterDot(i) && (first == 0 || toks_[first].newlineBefore || is(first - 1, Tok::LBrace) ||
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
  switch (toks_[i - 1].kind) {
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
    return toks_[i - 1].text == "throw";
  default:
    return false;
  }
}

bool FilePort::chainEnds(size_t i) const {
  if (i >= toks_.size() || toks_[i].newlineBefore) {
    return true;
  }
  switch (toks_[i].kind) {
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
  for (size_t i = 0; i < toks_.size(); ++i) {
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
             (is(p.first, Tok::TemplateHead) && close_[p.first] + 1 == p.second);
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
        out += templateText(toks_[from].text);
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
  for (size_t i = 0; i < toks_.size(); ++i) {
    const Token &t = toks_[i];
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
      if (is(j, Tok::LBrace) && close_[j] != std::string::npos) {
        lists.push_back({j, close_[j]});
      }
    }
    bool inList = llvm::any_of(lists, [&](auto &l) { return i > l.first && i < l.second; });
    if (afterDot(i) && !isName(i, "prototype") && !isName(i, "__proto__")) {
      continue;
    }

    if (t.is(Tok::KwAs) && !inList && i > 0 && isOperandEnd(toks_[i - 1])) {
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
      if (++functions[toks_[i + 1].text.str()] == 2) {
        note(i, "several signatures with one body ('" + toks_[i + 1].text.str() +
                    "') are not supported for plain functions (docs/overloads.md): keep one "
                    "signature with unions, or give each its own function");
      }
      continue;
    }
    if (!t.is(Tok::Identifier)) {
      continue;
    }
    llvm::StringRef name = t.text;
    if (name == "satisfies" && i > 0 && isOperandEnd(toks_[i - 1])) {
      note(i, "'satisfies' is not supported: declare the type of the variable instead, or "
              "remove it");
    } else if (name == "asserts" && i > 0 && is(i - 1, Tok::Colon) && is(i + 1, Tok::Identifier) &&
               !t.newlineBefore) {
      note(i, "'asserts' is not supported (docs/narrowing.md): there are no exceptions, so a "
              "function cannot stop its caller; write '" +
                  toks_[i + 1].text.str() +
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
                  name.str() + " " + toks_[i + 1].text.str() +
                  "()' a method, or keep the value in a field");
    } else if (name == "prototype" || name == "__proto__") {
      note(i, "prototypes are not supported (docs/objects.md): declare the methods in a class");
    } else if (name == "Object" && is(i + 1, Tok::Dot) &&
               (isName(i + 2, "setPrototypeOf") || isName(i + 2, "getPrototypeOf") ||
                isName(i + 2, "create") || isName(i + 2, "defineProperty") ||
                isName(i + 2, "defineProperties") || isName(i + 2, "freeze"))) {
      note(i, "'Object." + toks_[i + 2].text.str() +
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
      note(i, "'declare " + toks_[i + 1].text.str() + "' is not supported (docs/modules.md)");
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
      std::string spec = stringValue(toks_[i + 1]);
      llvm::StringRef s = spec;
      std::string package, sub;
      if (s.starts_with("npm:")) {
        note(i + 1, "npm packages are not supported (docs/packages.md): take the package from "
                    "JSR or git, adapted for tz, or do without it");
      } else if (!s.starts_with(".") && !s.contains(':') && !isBuiltinModule(s) &&
                 s != "neverthrow" && splitPackageSpecifier(s, package, sub) &&
                 !ctx_.jsrDependencies.count(package) && notedPackages_.insert(package).second) {
        note(i + 1, "'" + package +
                        "' is another package: add it to tz.json once it is adapted for tz "
                        "(docs/packages.md), or do without it");
      }
    }
  }
  for (size_t i = 1; i < toks_.size(); ++i) {
    if ((is(i, Tok::TemplateHead) || is(i, Tok::TemplateString)) &&
        (is(i - 1, Tok::Pipe) || (is(i - 1, Tok::Assign) && i >= 3 && isName(i - 3, "type")))) {
      note(i, "template literal types are not supported: write 'string'");
    }
  }
}

}
