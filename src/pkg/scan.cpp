#include "pkg/scan.h"

#include "lex/lexer.h"
#include "support/diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

// Whether a '/' after `prev` starts a regex literal rather than dividing.
bool regexMayFollow(const Token *prev) {
  if (!prev)
    return true;
  switch (prev->kind) {
  case Tok::Identifier:
    return prev->text == "throw" || prev->text == "yield" ||
           prev->text == "await" || prev->text == "of" || prev->text == "instanceof";
  case Tok::IntLiteral:
  case Tok::FloatLiteral:
  case Tok::StringLiteral:
  case Tok::TemplateString:
  case Tok::TemplateTail:
  case Tok::RegexLiteral:
  case Tok::RParen:
  case Tok::RBracket:
  case Tok::PlusPlus:
  case Tok::MinusMinus:
  case Tok::KwTrue:
  case Tok::KwFalse:
    return false;
  default:
    return true;
  }
}

} // namespace

std::vector<Token> tokenize(llvm::StringRef text) {
  llvm::SourceMgr sourceMgr;
  Diagnostics diags(sourceMgr);
  diags.setMuted(true);
  Lexer lexer(text, diags);
  std::vector<Token> tokens;
  while (true) {
    Token tok = lexer.next();
    if ((tok.is(Tok::Slash) || tok.is(Tok::SlashAssign)) &&
        regexMayFollow(tokens.empty() ? nullptr : &tokens.back()))
      tok = lexer.regex(tok.text.data(), tok.newlineBefore);
    if (tok.is(Tok::Eof))
      break;
    tokens.push_back(tok);
  }
  return tokens;
}

std::string stringValue(const Token &tok) {
  llvm::StringRef raw = tok.text;
  if (raw.size() < 2)
    return "";
  raw = raw.drop_front().drop_back();
  std::string out;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      ++i;
      char e = raw[i];
      out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
    } else {
      out += raw[i];
    }
  }
  return out;
}

std::vector<ImportRef> importSpecifiers(const std::vector<Token> &tokens, llvm::StringRef text) {
  std::vector<ImportRef> refs;
  auto add = [&](const Token &tok) {
    refs.push_back({stringValue(tok), static_cast<size_t>(tok.text.data() - text.data())});
  };
  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token &tok = tokens[i];
    if (!tok.is(Tok::Identifier) || (tok.text != "import" && tok.text != "export"))
      continue;
    if (i > 0 && (tokens[i - 1].is(Tok::Dot) || tokens[i - 1].is(Tok::QuestionDot)))
      continue;
    bool isImport = tok.text == "import";
    if (i + 1 < tokens.size() && isImport && tokens[i + 1].is(Tok::StringLiteral)) {
      add(tokens[i + 1]); // import "x"
      continue;
    }
    // The clause up to `from "x"`; braces must close before it.
    int depth = 0;
    for (size_t j = i + 1; j < tokens.size() && j < i + 2000; ++j) {
      const Token &t = tokens[j];
      if (t.is(Tok::LBrace)) {
        ++depth;
      } else if (t.is(Tok::RBrace)) {
        if (--depth < 0)
          break;
        // `export { a }` without `from` ends here.
        if (depth == 0 && !isImport &&
            !(j + 1 < tokens.size() && tokens[j + 1].is(Tok::Identifier) &&
              tokens[j + 1].text == "from"))
          break;
      } else if (depth == 0 && (t.is(Tok::Semi) || t.is(Tok::Assign) || t.is(Tok::LParen))) {
        break;
      } else if (depth == 0 && t.is(Tok::Identifier) && t.text == "from") {
        if (j + 1 < tokens.size() && tokens[j + 1].is(Tok::StringLiteral))
          add(tokens[j + 1]);
        break;
      } else if (depth == 0 && !isImport && j == i + 1 && !t.is(Tok::Star) &&
                 !(t.is(Tok::Identifier) && t.text == "type")) {
        break; // `export const ...`: a declaration
      }
    }
  }
  return refs;
}

std::string resolveRelative(llvm::StringRef fromFile, llvm::StringRef spec) {
  std::vector<std::string> candidates;
  llvm::StringRef last = path::filename(spec, path::Style::posix);
  llvm::StringRef ext = path::extension(spec, path::Style::posix);
  if (spec.ends_with("/") || last == "." || last == "..")
    candidates.push_back((spec.rtrim('/') + "/index.ts").str());
  else if (spec.ends_with(".d.ts"))
    return "";
  else if (ext == ".ts")
    candidates.push_back(spec.str());
  else if (ext == ".js")
    candidates.push_back((spec.drop_back(3) + ".ts").str());
  else if (ext.empty() || !llvm::all_of(ext.drop_front(), llvm::isAlpha)) {
    candidates.push_back((spec + ".ts").str());
    candidates.push_back((spec + "/index.ts").str());
  }
  for (const std::string &candidate : candidates) {
    llvm::SmallString<256> full(path::parent_path(fromFile));
    path::append(full, candidate);
    path::remove_dots(full, /*remove_dot_dot=*/true);
    if (fs::is_regular_file(full))
      return std::string(full);
  }
  return "";
}

void reachableFiles(const std::vector<std::string> &entries, std::vector<std::string> &files,
                    std::vector<std::string> &external) {
  llvm::StringSet<> seen, seenExternal;
  std::vector<std::string> work(entries.rbegin(), entries.rend());
  while (!work.empty()) {
    std::string file = work.back();
    work.pop_back();
    llvm::SmallString<256> real;
    if (fs::real_path(file, real))
      real = file;
    if (!seen.insert(real).second)
      continue;
    auto buffer = llvm::MemoryBuffer::getFile(file);
    if (!buffer)
      continue;
    files.push_back(file);
    llvm::StringRef text = (*buffer)->getBuffer();
    std::vector<ImportRef> refs = importSpecifiers(tokenize(text), text);
    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
      llvm::StringRef spec = it->specifier;
      if (spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == "..") {
        std::string target = resolveRelative(file, spec);
        if (!target.empty())
          work.push_back(target);
      } else if (seenExternal.insert(spec).second) {
        external.push_back(spec.str());
      }
    }
  }
}

} // namespace tz::pkg
