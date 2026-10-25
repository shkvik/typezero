// tz port: a copy of a TypeScript library with what tz does not accept
// changed where that can be done mechanically, and a list of the rest
// (docs/packages.md, section 2). It reads tokens, not a syntax tree: the code
// is TypeScript that tz's parser may refuse, and the changes are local.

#include "pkg/commands.h"

#include "ast/ast.h"
#include "pkg/fetch.h"
#include "pkg/json.h"
#include "pkg/project.h"
#include "pkg/scan.h"
#include "pkg/semver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

const char kSynopsis[] =
    "tz port <package> [-o <directory>] [--in-place] [--entry <file>]...\n\n"
    "  <package> is a directory, jsr:@scope/name[@range], github:user/repo[@tag] or\n"
    "  git:<address>[@tag]. The adapted copy goes to <directory> (default: the\n"
    "  package's name); --in-place changes a directory itself. --entry names the\n"
    "  files the package exports when it does not say.";

// What the package of the file being ported knows of the others.
struct PortContext {
  std::string packageName; // "ms"
  // Real path of each file -> the name of its default export, which becomes
  // a named one.
  std::map<std::string, std::string> defaultNames;
  // Packages its files import with "jsr:" -> where they come from, for its
  // tz.json.
  std::map<std::string, std::string> jsrDependencies;
};

// The changes made, by kind, for the summary.
using Counts = std::map<std::string, unsigned>;

struct Edit {
  size_t begin, end;
  std::string text;
  const char *kind; // for Counts, or null
  std::string note; // a note at the edit, for what still needs a look
};

struct Note {
  size_t offset;
  std::string message;
};

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

std::string capitalize(llvm::StringRef s) {
  std::string out = s.str();
  if (!out.empty())
    out[0] = llvm::toUpper(out[0]);
  return out;
}

// A name for the default export of `file`: after the file, or after the
// package for its index.ts.
std::string derivedName(llvm::StringRef file, llvm::StringRef packageDir,
                        llvm::StringRef packageName) {
  std::string stem = path::stem(file).str();
  llvm::StringRef dir = path::parent_path(file);
  if (stem == "index" || stem == "mod" || stem == "main") {
    llvm::StringRef parent = path::filename(dir);
    stem = dir == packageDir || parent == "src" || parent == "lib" ? packageName.str()
                                                                   : parent.str();
  }
  std::string name;
  bool upper = false;
  for (char c : stem) {
    if (!llvm::isAlnum(c)) {
      upper = !name.empty();
      continue;
    }
    name += upper ? llvm::toUpper(c) : c;
    upper = false;
  }
  if (name.empty() || llvm::isDigit(name[0]))
    name = "_" + name;
  return name;
}

// A string literal's text as the text of a template literal.
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

class FilePort {
public:
  FilePort(std::string file, std::string text, const PortContext &ctx, Counts &counts)
      : file_(std::move(file)), text_(std::move(text)), ctx_(ctx), counts_(counts) {}

  void run();
  const std::string &text() const { return text_; }
  std::vector<Note> &notes() { return notes_; }

private:
  void retokenize();
  size_t at(size_t i) const { return static_cast<size_t>(toks_[i].text.data() - text_.data()); }
  size_t endOf(size_t i) const { return at(i) + toks_[i].text.size(); }
  bool is(size_t i, Tok kind) const { return i < toks_.size() && toks_[i].kind == kind; }
  bool isName(size_t i, llvm::StringRef name) const {
    return is(i, Tok::Identifier) && toks_[i].text == name;
  }
  bool afterDot(size_t i) const {
    return i > 0 && (is(i - 1, Tok::Dot) || is(i - 1, Tok::QuestionDot));
  }
  void add(size_t begin, size_t end, std::string text, const char *kind, std::string note = "") {
    edits_.push_back({begin, end, std::move(text), kind, std::move(note)});
  }
  // Applies the edits that do not overlap an earlier one; the others wait
  // for the next pass. Returns whether anything changed.
  bool apply();

  // Pass 0: imports and exports, which change several places at once.
  void modules();
  void exportDefault(size_t i);
  void exportList(size_t open, size_t close);
  void importDecl(size_t i);
  // The members `name.x` the file uses, edited to plain names; false if it
  // uses `name` otherwise.
  bool members(const std::string &name, size_t skipBegin, size_t skipEnd,
               std::vector<std::string> &items);
  // Later passes: one token at a time, and '+' with strings.
  void tokens();
  void concatenations();
  size_t operand(size_t i) const;
  size_t unary(size_t i) const;
  bool chainMayStart(size_t i) const;
  bool chainEnds(size_t i) const;
  // The last pass: what is left for a person.
  void manual();
  void note(size_t i, const std::string &message) { notes_.push_back({at(i), message}); }

  std::string file_;
  std::string text_;
  const PortContext &ctx_;
  Counts &counts_;
  std::vector<Token> toks_;
  std::vector<size_t> close_; // of each ( [ { and template head; npos if none
  std::vector<Edit> edits_;
  std::vector<Note> notes_;
  // Names imported from a package that already gave a note.
  llvm::StringSet<> notedPackages_;
};

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
    Tok want = k == Tok::RParen     ? Tok::LParen
               : k == Tok::RBracket ? Tok::LBracket
               : k == Tok::RBrace   ? Tok::LBrace
               : k == Tok::TemplateTail ? Tok::TemplateHead
                                        : Tok::Eof;
    if (want == Tok::Eof)
      continue;
    if (!open.empty() && toks_[open.back()].kind == want) {
      close_[open.back()] = i;
      open.pop_back();
    }
  }
}

bool FilePort::apply() {
  if (edits_.empty())
    return false;
  std::stable_sort(edits_.begin(), edits_.end(),
                   [](const Edit &a, const Edit &b) { return a.begin < b.begin; });
  std::vector<const Edit *> taken;
  size_t last = 0;
  for (const Edit &edit : edits_) {
    if (edit.begin < last)
      continue;
    taken.push_back(&edit);
    last = std::max(last, edit.end);
  }
  // Where an offset of the old text is in the new one.
  auto shift = [&](size_t offset) {
    long long delta = 0;
    for (const Edit *edit : taken) {
      if (edit->begin >= offset && !(edit->begin == offset && edit->end == offset))
        break;
      if (offset < edit->end)
        return static_cast<size_t>(static_cast<long long>(edit->begin) + delta);
      delta += static_cast<long long>(edit->text.size()) -
               static_cast<long long>(edit->end - edit->begin);
    }
    return static_cast<size_t>(static_cast<long long>(offset) + delta);
  };
  for (Note &n : notes_)
    n.offset = shift(n.offset);
  std::string out;
  size_t from = 0;
  long long delta = 0;
  for (const Edit *edit : taken) {
    out.append(text_, from, edit->begin - from);
    if (!edit->note.empty())
      notes_.push_back({static_cast<size_t>(static_cast<long long>(edit->begin) + delta),
                        edit->note});
    out += edit->text;
    delta += static_cast<long long>(edit->text.size()) -
             static_cast<long long>(edit->end - edit->begin);
    from = edit->end;
    if (edit->kind)
      ++counts_[edit->kind];
  }
  out.append(text_, from, std::string::npos);
  text_ = std::move(out);
  edits_.clear();
  return true;
}

void FilePort::run() {
  retokenize();
  modules();
  if (apply())
    retokenize();
  // A change may make another one possible: a '+' inside a '+' becomes a
  // template inside a template on the next pass.
  for (int pass = 0; pass < 8; ++pass) {
    tokens();
    concatenations();
    if (!apply())
      break;
    retokenize();
  }
  manual();
  std::stable_sort(notes_.begin(), notes_.end(),
                   [](const Note &a, const Note &b) { return a.offset < b.offset; });
}

// --- imports and exports ------------------------------------------------------

void FilePort::modules() {
  for (size_t i = 0; i < toks_.size(); ++i) {
    if (afterDot(i))
      continue;
    if (isName(i, "export")) {
      if (is(i + 1, Tok::KwDefault)) {
        exportDefault(i);
      } else if (isName(i + 1, "type") && is(i + 2, Tok::LBrace)) {
        add(at(i + 1), at(i + 2), "", "'import type' and 'export type' lost 'type'");
        exportList(i + 2, close_[i + 2]);
      } else if (is(i + 1, Tok::LBrace)) {
        exportList(i + 1, close_[i + 1]);
      }
    } else if (isName(i, "import")) {
      importDecl(i);
    }
  }
}

void FilePort::exportDefault(size_t i) {
  const char *kind = "'export default' became a named export";
  size_t def = i + 1, k = i + 2;
  if (k >= toks_.size())
    return;
  llvm::SmallString<256> real;
  fs::real_path(file_, real);
  auto known = ctx_.defaultNames.find(std::string(real));
  std::string name = known != ctx_.defaultNames.end() ? known->second : "";
  // `export default function f`, `export default async function* f`,
  // `export default class C`, `export default abstract class C`.
  size_t keyword = k;
  if (isName(keyword, "async") && is(keyword + 1, Tok::KwFunction))
    ++keyword;
  if (isName(keyword, "abstract") && isName(keyword + 1, "class"))
    ++keyword;
  if (is(keyword, Tok::KwFunction) || isName(keyword, "class") || isName(keyword, "interface")) {
    size_t last = keyword;
    if (is(last + 1, Tok::Star))
      ++last;
    bool named = is(last + 1, Tok::Identifier) && toks_[last + 1].text != "extends" &&
                 toks_[last + 1].text != "implements";
    if (named) {
      add(at(def), at(k), "", kind);
    } else {
      // Anonymous: it gets the name importers will use, `function f(` and
      // `class C {`.
      std::string words = text_.substr(at(k), endOf(last) - at(k));
      if (last + 1 < toks_.size())
        add(at(def), at(last + 1),
            words + " " + name + (is(last + 1, Tok::LParen) ? "" : " "), kind);
      else
        add(at(def), endOf(last), words + " " + name, kind);
    }
    return;
  }
  // `export default foo`
  if (is(k, Tok::Identifier) &&
      (k + 1 >= toks_.size() || is(k + 1, Tok::Semi) || toks_[k + 1].newlineBefore)) {
    add(at(i), endOf(k), "export { " + toks_[k].text.str() + " }", kind);
    return;
  }
  // `export default <expression>`
  add(at(def), endOf(def), "const " + name + " =", kind);
}

void FilePort::exportList(size_t open, size_t close) {
  if (close == std::string::npos)
    return;
  // `export { a as default }` and `export { default } from "./x"`.
  std::string target;
  if (isName(close + 1, "from") && is(close + 2, Tok::StringLiteral)) {
    std::string spec = stringValue(toks_[close + 2]);
    std::string file = resolveRelative(file_, spec);
    llvm::SmallString<256> real;
    if (!file.empty() && !fs::real_path(file, real))
      if (auto it = ctx_.defaultNames.find(std::string(real)); it != ctx_.defaultNames.end())
        target = it->second;
  }
  for (size_t j = open + 1; j < close; ++j) {
    if (isName(j, "type") && is(j + 1, Tok::Identifier) && !is(j + 1, Tok::Comma))
      add(at(j), at(j + 1), "", "'import type' and 'export type' lost 'type'");
    if (is(j, Tok::KwAs) && is(j + 1, Tok::KwDefault)) {
      add(endOf(j - 1), endOf(j + 1), "", "'export default' became a named export");
      ++j;
    } else if (is(j, Tok::KwDefault) && !target.empty()) {
      bool renamed = is(j + 1, Tok::KwAs) && is(j + 2, Tok::KwDefault);
      add(at(j), renamed ? endOf(j + 2) : endOf(j), target,
          "'export default' became a named export");
      if (renamed)
        j += 2;
    }
  }
}

bool FilePort::members(const std::string &name, size_t skipBegin, size_t skipEnd,
                       std::vector<std::string> &items) {
  // A member that is also a name of the file is imported under another one.
  llvm::StringSet<> own;
  for (size_t j = 0; j < toks_.size(); ++j)
    if ((j < skipBegin || j >= skipEnd) && is(j, Tok::Identifier) && !afterDot(j))
      own.insert(toks_[j].text);
  std::vector<Edit> uses;
  std::set<std::string> seen;
  for (size_t j = 0; j < toks_.size(); ++j) {
    if (j >= skipBegin && j < skipEnd)
      continue;
    if (!isName(j, name) || afterDot(j))
      continue;
    // `{ name: ... }` is a key, not a use.
    if (is(j + 1, Tok::Colon) && j > 0 && (is(j - 1, Tok::LBrace) || is(j - 1, Tok::Comma)))
      continue;
    if (!is(j + 1, Tok::Dot) || !is(j + 2, Tok::Identifier))
      return false;
    std::string member = toks_[j + 2].text.str();
    std::string local = own.contains(member) ? name + capitalize(member) : member;
    uses.push_back({at(j), endOf(j + 2), local, nullptr, ""});
    if (seen.insert(member).second)
      items.push_back(local == member ? member : member + " as " + local);
  }
  for (Edit &use : uses)
    edits_.push_back(std::move(use));
  return true;
}

void FilePort::importDecl(size_t i) {
  if (is(i + 1, Tok::StringLiteral) || is(i + 1, Tok::LParen))
    return; // for the manual pass
  // The clause up to `from "x"`.
  size_t from = std::string::npos;
  for (size_t j = i + 1; j < toks_.size() && j < i + 2000; ++j) {
    if (is(j, Tok::LBrace) && close_[j] != std::string::npos) {
      j = close_[j];
      continue;
    }
    if (is(j, Tok::Semi) || is(j, Tok::Assign))
      return;
    if (isName(j, "from") && is(j + 1, Tok::StringLiteral)) {
      from = j;
      break;
    }
  }
  if (from == std::string::npos)
    return;
  const char *typeKind = "'import type' and 'export type' lost 'type'";
  size_t c = i + 1;
  if (isName(c, "type") && !isName(c + 1, "from") && !is(c + 1, Tok::Comma)) {
    add(at(c), at(c + 1), "", typeKind);
    ++c;
  }
  size_t clauseBegin = c;
  std::string spec = stringValue(toks_[from + 1]);
  llvm::StringRef specRef(spec);

  // "jsr:@std/path@^1/join" becomes "@std/path/join", a dependency in
  // tz.json.
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
  // The names in braces, without 'type'.
  std::vector<std::string> items;
  if (is(c, Tok::LBrace) && close_[c] != std::string::npos) {
    bool rewrite = !defaultName.empty() || !ns.empty();
    std::string item;
    for (size_t j = c + 1; j < close_[c]; ++j) {
      if (isName(j, "type") && is(j + 1, Tok::Identifier)) {
        if (!rewrite)
          add(at(j), at(j + 1), "", typeKind);
        else
          ++counts_[typeKind];
        continue;
      }
      if (is(j, Tok::Comma)) {
        if (!item.empty())
          items.push_back(item);
        item.clear();
        continue;
      }
      item += (item.empty() ? "" : " ") + toks_[j].text.str();
    }
    if (!item.empty())
      items.push_back(item);
  }
  if (defaultName.empty() && ns.empty())
    return;

  bool relative = specRef.starts_with(".");
  if (!defaultName.empty()) {
    std::string exported;
    if (relative) {
      std::string target = resolveRelative(file_, spec);
      llvm::SmallString<256> real;
      if (!target.empty() && !fs::real_path(target, real))
        if (auto it = ctx_.defaultNames.find(std::string(real)); it != ctx_.defaultNames.end())
          exported = it->second;
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
    notes_.push_back({at(clauseBegin), "'import * as " + ns +
                                           "' is not supported, and '" + ns +
                                           "' is used as a whole: import by name what it uses"});
    return;
  }
  add(at(clauseBegin), at(from), "{ " + llvm::join(items, ", ") + " } ",
      !ns.empty() ? "'import * as' became named imports" : "default imports became named imports");
}

// --- tokens ---------------------------------------------------------------------

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
      if (is(i + 1, Tok::IntLiteral) && toks_[i + 1].text == "0")
        add(at(i), endOf(i + 1), "undefined", "'null' and 'void 0' became 'undefined'");
      break;
    case Tok::Identifier:
      if (afterDot(i))
        break;
      if (t.text == "null") {
        if (is(i + 1, Tok::Colon) && i > 0 && (is(i - 1, Tok::LBrace) || is(i - 1, Tok::Comma)))
          break; // a key
        // A Json holds null itself (docs/modules.md): `Json | null` is Json,
        // and `const j: Json = null` stays as it is.
        const char *json = "'| null' next to Json was dropped: a Json holds null itself";
        if (i >= 2 && is(i - 1, Tok::Pipe) && isName(i - 2, "Json")) {
          add(endOf(i - 2), endOf(i), "", json);
          break;
        }
        if (is(i + 1, Tok::Pipe) && isName(i + 2, "Json")) {
          add(at(i), at(i + 2), "", json);
          break;
        }
        if (i >= 2 && is(i - 1, Tok::Assign) && isName(i - 2, "Json"))
          break;
        const char *kind = "'null' and 'void 0' became 'undefined'";
        // `T | null | undefined` has one 'undefined' afterwards.
        if (i >= 2 && is(i - 1, Tok::Pipe) && isName(i - 2, "undefined"))
          add(endOf(i - 2), endOf(i), "", kind);
        else if (is(i + 1, Tok::Pipe) && isName(i + 2, "undefined"))
          add(at(i), at(i + 2), "", kind);
        else
          add(at(i), endOf(i), "undefined", kind);
      } else if (t.text == "any" && i > 0 && !is(i + 1, Tok::Colon) && !is(i + 1, Tok::Assign) &&
                 !is(i + 1, Tok::LParen) && !is(i + 1, Tok::Dot) &&
                 (is(i - 1, Tok::Colon) || is(i - 1, Tok::Less) || is(i - 1, Tok::Comma) ||
                  is(i - 1, Tok::Pipe) || is(i - 1, Tok::Amp) || is(i - 1, Tok::Arrow) ||
                  is(i - 1, Tok::LParen) || is(i - 1, Tok::LBracket) || is(i - 1, Tok::KwAs) ||
                  isName(i - 1, "extends") || isName(i - 1, "keyof") ||
                  isName(i - 1, "readonly") || isName(i - 1, "is") ||
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

// One operand of '*' and the like: prefix operators, a primary expression and
// what follows it (members, calls, indexes, '!'). The index after it, or 0
// if it is something this rule does not take apart (a function, an object).
size_t FilePort::unary(size_t i) const {
  size_t n = toks_.size();
  while (i < n && (is(i, Tok::Bang) || is(i, Tok::Tilde) || is(i, Tok::Minus) ||
                   is(i, Tok::Plus) || is(i, Tok::KwTypeof) || is(i, Tok::KwVoid) ||
                   is(i, Tok::KwDelete) || isName(i, "await")))
    ++i;
  if (i >= n)
    return 0;
  auto closed = [&](size_t j) { return close_[j] == std::string::npos ? 0 : close_[j] + 1; };
  switch (toks_[i].kind) {
  case Tok::Identifier: {
    llvm::StringRef name = toks_[i].text;
    if (name == "function" || name == "class" || name == "async" || name == "yield" ||
        is(i + 1, Tok::Arrow))
      return 0;
    ++i;
    break;
  }
  case Tok::KwNew:
    if (!is(i + 1, Tok::Identifier))
      return 0;
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
    if (!after || is(after, Tok::Arrow))
      return 0;
    i = after;
    break;
  }
  default:
    return 0;
  }
  while (i && i < n) {
    const Token &t = toks_[i];
    if (t.is(Tok::Dot) || t.is(Tok::QuestionDot)) {
      if (is(i + 1, Tok::Identifier))
        i += 2;
      else if (t.is(Tok::QuestionDot) && (is(i + 1, Tok::LParen) || is(i + 1, Tok::LBracket)))
        i = closed(i + 1);
      else
        return 0;
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

// An operand of '+': unary operands joined by '*', '/', '%' and '**'.
size_t FilePort::operand(size_t i) const {
  size_t end = unary(i);
  while (end && (is(end, Tok::Star) || is(end, Tok::Slash) || is(end, Tok::Percent) ||
                 is(end, Tok::StarStar)))
    end = unary(end + 1);
  return end;
}

bool FilePort::chainMayStart(size_t i) const {
  if (i == 0)
    return true;
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
  if (i >= toks_.size() || toks_[i].newlineBefore)
    return true;
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

// `"x = " + x + "!"` becomes `x = ${x}!`. A string literal makes the rest of
// the chain strings, so only what comes before the first one stays one
// expression: `a + b + "!"` is `${a + b}!`.
void FilePort::concatenations() {
  for (size_t i = 0; i < toks_.size(); ++i) {
    if (!chainMayStart(i))
      continue;
    size_t end = operand(i);
    if (!end)
      continue;
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
    if (!ok || ops.empty() || !chainEnds(end))
      continue;
    auto literal = [&](const std::pair<size_t, size_t> &p) {
      return (p.second == p.first + 1 &&
              (is(p.first, Tok::StringLiteral) || is(p.first, Tok::TemplateString))) ||
             (is(p.first, Tok::TemplateHead) && close_[p.first] + 1 == p.second);
    };
    size_t first = 0;
    while (first < parts.size() && !literal(parts[first]))
      ++first;
    bool allLiteral = llvm::all_of(parts, literal);
    bool plusAfter = true;
    for (size_t k = first == 0 ? 0 : first - 1; k < ops.size(); ++k)
      plusAfter &= is(ops[k], Tok::Plus);
    // Comments between the tokens would be lost.
    bool quiet = true;
    for (size_t k = i; k + 1 < end; ++k)
      if (!llvm::StringRef(text_).slice(endOf(k), at(k + 1)).trim().empty())
        quiet = false;
    if (first == parts.size() || allLiteral || !plusAfter || !quiet) {
      i = end - 1;
      continue;
    }
    auto source = [&](size_t from, size_t to) {
      return text_.substr(at(from), endOf(to - 1) - at(from));
    };
    std::string out = "`";
    if (first > 0)
      out += "${" + source(parts[0].first, parts[first - 1].second) + "}";
    for (size_t k = first; k < parts.size(); ++k) {
      auto [from, to] = parts[k];
      if (is(from, Tok::StringLiteral) && to == from + 1)
        out += templateText(toks_[from].text);
      else if (literal(parts[k]))
        out += llvm::StringRef(source(from, to)).drop_front().drop_back().str();
      else
        out += "${" + source(from, to) + "}";
    }
    out += "`";
    add(at(i), endOf(end - 1), out, "'+' with a string became a template literal");
    i = end - 1;
  }
}

// --- what is left -----------------------------------------------------------------

void FilePort::manual() {
  // The braces of imports and exports, where 'as' renames.
  std::vector<std::pair<size_t, size_t>> lists;
  std::map<std::string, unsigned> functions;
  for (size_t i = 0; i < toks_.size(); ++i) {
    const Token &t = toks_[i];
    if ((isName(i, "import") || isName(i, "export")) && !afterDot(i)) {
      size_t j = i + 1;
      if (isName(j, "type"))
        ++j;
      if (isName(j, "import") || isName(j, "export"))
        continue;
      if (is(j, Tok::Identifier) && is(j + 1, Tok::Comma))
        j += 2;
      if (is(j, Tok::LBrace) && close_[j] != std::string::npos)
        lists.push_back({j, close_[j]});
    }
    bool inList = llvm::any_of(lists, [&](auto &l) { return i > l.first && i < l.second; });
    if (afterDot(i) && !isName(i, "prototype") && !isName(i, "__proto__"))
      continue;

    if (t.is(Tok::KwAs) && !inList && i > 0 && isOperandEnd(toks_[i - 1])) {
      if (is(i + 1, Tok::KwConst))
        note(i, "'as const' is not supported: declare the type of the value instead, e.g. a "
                "union of literal types");
      else if (isName(i + 1, "unknown") && is(i + 2, Tok::KwAs))
        note(i, "'as unknown as' is not supported: tz has no casts; check the value (typeof, "
                "instanceof, 'in') to narrow it, or give it the right type where it is made");
      else
        note(i, "'as' is not supported: remove it; if the value's type is wider than written, "
                "narrow it with a check (typeof, instanceof, 'in', '!= undefined')");
      continue;
    }
    if (t.is(Tok::KwNew) && isName(i + 1, "Function") && is(i + 2, Tok::LParen)) {
      note(i, "code made at run time ('new Function', 'eval') is not supported");
      continue;
    }
    if (t.is(Tok::KwFunction) && is(i + 1, Tok::Identifier)) {
      // Several signatures and one body, as TypeScript overloads plain
      // functions.
      if (++functions[toks_[i + 1].text.str()] == 2)
        note(i, "several signatures with one body ('" + toks_[i + 1].text.str() +
                    "') are not supported for plain functions (docs/overloads.md): keep one "
                    "signature with unions, or give each its own function");
      continue;
    }
    if (!t.is(Tok::Identifier))
      continue;
    llvm::StringRef name = t.text;
    if (name == "satisfies" && i > 0 && isOperandEnd(toks_[i - 1])) {
      note(i, "'satisfies' is not supported: declare the type of the variable instead, or "
              "remove it");
    } else if ((name == "get" || name == "set") &&
               (is(i + 1, Tok::Identifier) || is(i + 1, Tok::StringLiteral) ||
                is(i + 1, Tok::LBracket)) &&
               is(i + 2, Tok::LParen) &&
               (i == 0 || t.newlineBefore || is(i - 1, Tok::LBrace) || is(i - 1, Tok::Semi) ||
                is(i - 1, Tok::RBrace) || is(i - 1, Tok::Comma) || isName(i - 1, "static") ||
                isName(i - 1, "public") || isName(i - 1, "private") ||
                isName(i - 1, "protected") || isName(i - 1, "override"))) {
      note(i, "getters and setters are not supported (docs/objects.md): reading a field never "
              "runs code; make '" + name.str() + " " + toks_[i + 1].text.str() +
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
  // Template literal types: `${number}px` where a type is written.
  for (size_t i = 1; i < toks_.size(); ++i)
    if ((is(i, Tok::TemplateHead) || is(i, Tok::TemplateString)) &&
        (is(i - 1, Tok::Pipe) || (is(i - 1, Tok::Assign) && i >= 3 && isName(i - 3, "type"))))
      note(i, "template literal types are not supported: write 'string'");
}

// --- the command --------------------------------------------------------------------

bool readFile(llvm::StringRef file, std::string &text) {
  auto buffer = llvm::MemoryBuffer::getFile(file, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!buffer) {
    report("cannot read '" + file + "': " + buffer.getError().message());
    return false;
  }
  text = (*buffer)->getBuffer().str();
  return true;
}

bool writeFile(llvm::StringRef file, llvm::StringRef text) {
  if (std::error_code ec = fs::create_directories(path::parent_path(file))) {
    report("cannot create '" + path::parent_path(file) + "': " + ec.message());
    return false;
  }
  std::error_code ec;
  llvm::raw_fd_ostream out(file, ec, fs::OF_None);
  if (!ec) {
    out << text;
    out.close();
    if (out.has_error()) {
      ec = out.error();
      out.clear_error();
    }
  }
  if (ec) {
    report("cannot write '" + file + "': " + ec.message());
    return false;
  }
  return true;
}

// The name of the default export of each file, before anything changes.
void findDefaultNames(const std::vector<std::string> &files, llvm::StringRef packageDir,
                      PortContext &ctx) {
  // `export { default } from "./x"` takes x's name: files are read until no
  // name changes.
  for (int round = 0; round < 4; ++round) {
    for (const std::string &file : files) {
      llvm::SmallString<256> real;
      if (fs::real_path(file, real) || ctx.defaultNames.count(std::string(real)))
        continue;
      std::string text;
      auto buffer = llvm::MemoryBuffer::getFile(file);
      if (!buffer)
        continue;
      text = (*buffer)->getBuffer().str();
      std::vector<Token> toks = tokenize(text);
      std::string name;
      for (size_t i = 0; i + 1 < toks.size() && name.empty(); ++i) {
        if (toks[i].kind != Tok::Identifier || toks[i].text != "export")
          continue;
        if (toks[i + 1].is(Tok::KwDefault)) {
          size_t k = i + 2;
          if (k < toks.size() && toks[k].text == "async")
            ++k;
          if (k < toks.size() && toks[k].text == "abstract")
            ++k;
          if (k < toks.size() && (toks[k].is(Tok::KwFunction) || toks[k].text == "class" ||
                                  toks[k].text == "interface")) {
            bool isClass = toks[k].text == "class";
            if (k + 1 < toks.size() && toks[k + 1].is(Tok::Star))
              ++k;
            if (k + 1 < toks.size() && toks[k + 1].is(Tok::Identifier) &&
                toks[k + 1].text != "extends" && toks[k + 1].text != "implements")
              name = toks[k + 1].text.str();
            else
              name = isClass ? capitalize(derivedName(file, packageDir, ctx.packageName))
                             : derivedName(file, packageDir, ctx.packageName);
          } else if (k < toks.size() && toks[k].is(Tok::Identifier) &&
                     (k + 1 == toks.size() || toks[k + 1].is(Tok::Semi) ||
                      toks[k + 1].newlineBefore)) {
            name = toks[k].text.str();
          } else {
            name = derivedName(file, packageDir, ctx.packageName);
          }
        } else if (toks[i + 1].is(Tok::LBrace)) {
          size_t j = i + 2;
          for (; j < toks.size() && !toks[j].is(Tok::RBrace); ++j) {
            if (toks[j].is(Tok::KwAs) && j + 1 < toks.size() && toks[j + 1].is(Tok::KwDefault))
              name = toks[j - 1].is(Tok::KwDefault) ? "" : toks[j - 1].text.str();
          }
          bool reexportsDefault = false;
          for (size_t k = i + 2; k < j; ++k)
            if (toks[k].is(Tok::KwDefault) && !(k > 0 && toks[k - 1].is(Tok::KwAs)) &&
                (k + 1 == j || toks[k + 1].is(Tok::Comma) ||
                 (toks[k + 1].is(Tok::KwAs) && k + 2 < j && toks[k + 2].is(Tok::KwDefault))))
              reexportsDefault = true;
          if (reexportsDefault && j + 2 < toks.size() && toks[j + 1].text == "from") {
            std::string target = resolveRelative(file, stringValue(toks[j + 2]));
            llvm::SmallString<256> targetReal;
            if (!target.empty() && !fs::real_path(target, targetReal))
              if (auto it = ctx.defaultNames.find(std::string(targetReal));
                  it != ctx.defaultNames.end())
                name = it->second;
          }
        }
      }
      if (!name.empty())
        ctx.defaultNames[std::string(real)] = name;
    }
  }
}

// What a package says it exports, as files relative to its directory.
std::vector<std::pair<std::string, std::string>> packageEntries(const std::string &dir,
                                                                const Manifest &manifest,
                                                                std::string &name,
                                                                std::string &version) {
  std::vector<std::pair<std::string, std::string>> entries = manifest.exports;
  if (entries.empty() && !manifest.main.empty())
    entries.push_back({".", manifest.main});
  if (name.empty())
    name = manifest.name;
  if (version.empty())
    version = manifest.version;
  // A package for npm: its name and version, and the .ts files its
  // package.json names.
  llvm::SmallString<256> packageJson(dir);
  path::append(packageJson, "package.json");
  if (auto buffer = llvm::MemoryBuffer::getFile(packageJson)) {
    JsonValue root;
    size_t at;
    std::string error;
    if (parseJson((*buffer)->getBuffer(), root, at, error)) {
      if (name.empty())
        name = root.getString("name");
      if (version.empty())
        version = root.getString("version");
      for (const char *field : {"source", "main", "module", "types"}) {
        llvm::StringRef file = root.getString(field);
        if (entries.empty() && file.ends_with(".ts") && !file.ends_with(".d.ts"))
          entries.push_back({".", file.ltrim("./").str()});
      }
    }
  }
  for (const char *candidate :
       {"mod.ts", "index.ts", "src/index.ts", "src/mod.ts", "lib/index.ts"}) {
    if (!entries.empty())
      break;
    llvm::SmallString<256> file(dir);
    path::append(file, candidate);
    if (fs::is_regular_file(file))
      entries.push_back({".", candidate});
  }
  return entries;
}

// The sources of what `tz port` is given, in the cache or on the disk.
bool fetchSource(llvm::StringRef arg, std::string &dir, std::string &name, std::string &version,
                 std::vector<std::pair<std::string, std::string>> &exports, bool &local) {
  std::string error;
  local = false;
  if (fs::is_directory(arg)) {
    llvm::SmallString<256> real;
    if (fs::real_path(arg, real)) {
      report("cannot read '" + arg + "'");
      return false;
    }
    dir = std::string(real);
    local = true;
    return true;
  }
  std::string spec = arg.starts_with("@") ? "jsr:" + arg.str() : arg.str();
  Source source;
  if (!parseSource(spec, "", source, error)) {
    report(error);
    return false;
  }
  if (source.kind == Source::Kind::Jsr) {
    JsrVersions versions;
    if (!jsrVersions(source.scope, source.name, versions, error)) {
      report(error.empty() ? "there is no package " + source.id() + " in the registry (" +
                                 jsrBaseUrl() + ")"
                           : error);
      return false;
    }
    Range range;
    Range::parse(source.range, range, error);
    const Version *best = nullptr;
    for (const JsrVersions::Entry &entry : versions.versions)
      if (!entry.yanked &&
          (source.range.empty() ? !entry.version.isPrerelease() : range.satisfies(entry.version)) &&
          (!best || *best < entry.version))
        best = &entry.version;
    if (!best) {
      report("the registry has no version of " + source.id() + " that fits '" + source.range +
             "'");
      return false;
    }
    JsrVersion meta;
    version = best->str();
    if (!jsrVersion(source.scope, source.name, version, meta, error) ||
        !jsrDownload(source.scope, source.name, version, meta, dir, error)) {
      report(error);
      return false;
    }
    name = source.shortName();
    exports = meta.exports;
    return true;
  }
  if (source.kind == Source::Kind::Git) {
    std::string commit;
    if (!gitResolve(source.url, source.ref, /*refresh=*/true, commit, error) ||
        !gitCheckout(source.url, commit, dir, error)) {
      report(error);
      return false;
    }
    version = llvm::StringRef(source.ref).ltrim("v").str();
    return true;
  }
  dir = source.dir;
  local = true;
  return true;
}

std::string displayPath(llvm::StringRef base, llvm::StringRef relative) {
  llvm::SmallString<256> shown(base);
  path::append(shown, relative);
  return path::convert_to_slash(shown);
}

} // namespace

int commandPort(llvm::ArrayRef<const char *> args) {
  std::string sourceArg, outArg;
  std::vector<std::string> entryArgs;
  bool inPlace = false;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg == "-o" || arg == "--entry") {
      if (i + 1 == args.size()) {
        llvm::errs() << "tz: '" << arg << "' needs a path\n\nusage: " << kSynopsis << "\n";
        return kExitUsage;
      }
      (arg == "-o" ? outArg : entryArgs.emplace_back()) = args[++i];
    } else if (arg == "--in-place") {
      inPlace = true;
    } else if (arg.starts_with("-") || !sourceArg.empty()) {
      llvm::errs() << "tz: unexpected argument '" << arg << "'\n\nusage: " << kSynopsis << "\n";
      return kExitUsage;
    } else {
      sourceArg = arg.str();
    }
  }
  if (sourceArg.empty()) {
    llvm::errs() << "tz: 'tz port' needs a package\n\nusage: " << kSynopsis << "\n";
    return kExitUsage;
  }

  std::string dir, name, version;
  std::vector<std::pair<std::string, std::string>> exports;
  bool local;
  if (!fetchSource(sourceArg, dir, name, version, exports, local))
    return kExitError;
  // Paths are compared with the files' real paths.
  llvm::SmallString<256> realDir;
  if (!fs::real_path(dir, realDir))
    dir = std::string(realDir);
  if (inPlace && !local) {
    report("--in-place changes a directory of this machine; '" + sourceArg + "' is not one");
    return kExitError;
  }
  Manifest manifest;
  if (!readPackageDir(dir, manifest))
    return kExitError;
  if (exports.empty())
    exports = packageEntries(dir, manifest, name, version);
  else
    packageEntries(dir, manifest, name, version);
  if (!entryArgs.empty()) {
    exports.clear();
    for (size_t i = 0; i < entryArgs.size(); ++i)
      exports.push_back({i == 0 ? std::string(".")
                                : "./" + path::stem(entryArgs[i], path::Style::posix).str(),
                         llvm::StringRef(entryArgs[i]).ltrim("./").str()});
  }
  if (name.empty())
    name = path::filename(dir).str();
  if (exports.empty()) {
    report("cannot tell which files " + name +
           " exports: it has no tz.json, jsr.json or package.json that names .ts files, and no "
           "mod.ts, index.ts or src/index.ts; name them with --entry <file>");
    return kExitError;
  }

  // Where the copy goes.
  std::string shortName = llvm::StringRef(name).substr(llvm::StringRef(name).rfind('/') + 1).str();
  // `out` as the report shows it.
  std::string out = inPlace ? sourceArg : outArg.empty() ? shortName : outArg;
  llvm::SmallString<256> outDir(inPlace ? dir : out);
  fs::make_absolute(outDir);
  if (!inPlace) {
    std::error_code ec;
    fs::directory_iterator it(outDir, ec);
    if (!ec && it != fs::directory_iterator()) {
      report("'" + out + "' is not empty; choose another directory with -o");
      return kExitError;
    }
  }

  // The files the exports reach, and the names of their default exports.
  std::vector<std::string> entries, files, external;
  for (const auto &[key, file] : exports) {
    llvm::SmallString<256> full(dir);
    path::append(full, file);
    if (!fs::is_regular_file(full)) {
      report("cannot find '" + file + "', which " + name + " exports as '" + key + "'");
      return kExitError;
    }
    entries.push_back(std::string(full));
  }
  reachableFiles(entries, files, external);
  PortContext ctx;
  ctx.packageName = derivedName(shortName + ".ts", "", "");
  findDefaultNames(files, dir, ctx);
  for (llvm::StringRef spec : external) {
    std::string package, source, subpath;
    if (splitJsrImport(spec, package, source, subpath))
      ctx.jsrDependencies[package] = source;
  }

  // Port each file.
  Counts counts;
  struct Ported {
    std::string relative;
    std::string text;
    std::vector<Note> notes;
  };
  std::vector<Ported> ported;
  for (const std::string &file : files) {
    llvm::SmallString<256> real;
    if (fs::real_path(file, real))
      real = file;
    llvm::StringRef relative = real;
    if (!relative.consume_front(dir) ||
        !(relative.starts_with("/") || relative.starts_with("\\"))) {
      report("'" + file + "' is outside " + dir + ", and is not copied");
      continue;
    }
    std::string text;
    if (!readFile(file, text))
      return kExitError;
    FilePort port(file, std::move(text), ctx, counts);
    port.run();
    ported.push_back({path::convert_to_slash(relative.drop_front()), port.text(),
                      std::move(port.notes())});
  }
  std::sort(ported.begin(), ported.end(),
            [](const Ported &a, const Ported &b) { return a.relative < b.relative; });
  for (const Ported &p : ported) {
    llvm::SmallString<256> target(outDir);
    path::append(target, p.relative);
    if (!writeFile(target, p.text))
      return kExitError;
  }
  // The license and the readme go with the code.
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    std::string file = path::filename(it->path()).lower();
    if (!inPlace && fs::is_regular_file(it->path()) &&
        (llvm::StringRef(file).starts_with("license") ||
         llvm::StringRef(file).starts_with("licence") ||
         llvm::StringRef(file).starts_with("copying") ||
         llvm::StringRef(file).starts_with("notice") ||
         llvm::StringRef(file).starts_with("readme"))) {
      std::string text;
      llvm::SmallString<256> target(outDir);
      path::append(target, path::filename(it->path()));
      if (!readFile(it->path(), text) || !writeFile(target, text))
        return kExitError;
    }
  }
  // A tz.json that says what the package is.
  llvm::SmallString<256> tzJson(outDir);
  path::append(tzJson, "tz.json");
  if (!inPlace || !fs::exists(tzJson)) {
    std::string json = "{\n  \"name\": " + jsonQuote(name);
    if (!version.empty())
      json += ",\n  \"version\": " + jsonQuote(version);
    if (exports.size() == 1 && exports[0].first == ".") {
      json += ",\n  \"main\": " + jsonQuote(exports[0].second);
    } else {
      json += ",\n  \"exports\": {";
      for (size_t i = 0; i < exports.size(); ++i)
        json += (i ? ",\n    " : "\n    ") + jsonQuote(exports[i].first) + ": " +
                jsonQuote("./" + exports[i].second);
      json += "\n  }";
    }
    json += ",\n  \"dependencies\": {";
    size_t i = 0;
    for (const auto &[alias, spec] : ctx.jsrDependencies)
      json += (i++ ? ",\n    " : "\n    ") + jsonQuote(alias) + ": " + jsonQuote(spec);
    json += i ? "\n  }\n}\n" : "}\n}\n";
    if (!writeFile(tzJson, json))
      return kExitError;
  }

  // The report.
  llvm::outs() << "ported " << name << (version.empty() ? "" : " " + version) << " into "
               << path::convert_to_slash(out) << " (" << ported.size()
               << (ported.size() == 1 ? " file" : " files") << ")\n";
  if (!counts.empty()) {
    llvm::outs() << "changed:\n";
    for (const auto &[kind, n] : counts)
      llvm::outs() << llvm::format("%5u  ", n) << kind << "\n";
  }
  size_t manualCount = 0;
  for (const Ported &p : ported)
    manualCount += p.notes.size();
  if (manualCount)
    llvm::outs() << "to change by hand (" << manualCount << "):\n";
  for (const Ported &p : ported)
    for (const Note &n : p.notes) {
      auto [line, column] = lineAndColumn(p.text, n.offset);
      llvm::outs() << "  " << displayPath(out, p.relative) << ":" << line << ":" << column
                   << ": " << n.message << "\n";
    }
  llvm::outs() << "then 'tz build " << displayPath(out, exports[0].second)
               << "' reports what else tz does not accept\n";
  return 0;
}

} // namespace tz::pkg
