#include "compiler/cli/syntax/syntax_command/syntax_command.h"
#include "compiler/cli/syntax/ast_dump/ast_dump.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/ast/ast_context/ast_context.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/source_file/source_file.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <chrono>
#include <thread>

namespace tz {

int SyntaxCommand::run(llvm::ArrayRef<const char *> words) const {
  if (words.empty()) {
    llvm::errs() << "usage: tz syntax tokens|tree|time <repeat> <file.tz | @list>...\n";
    return 2;
  }
  llvm::StringRef mode = words.front();
  if (mode == "time" && words.size() > 1) {
    int repeat = 1;
    llvm::StringRef(words[1]).getAsInteger(10, repeat);
    auto sources = this->readSources(words.drop_front(2));
    return this->timeSources(sources, repeat);
  }
  if (mode == "hold" || mode == "read") {
    auto sources = this->readSources(words.drop_front());
    return this->holdSources(sources, mode == "hold");
  }
  auto sources = this->readSources(words.drop_front());
  for (const SourceText &source : sources) {
    llvm::outs() << "== " << source.path << "\n";
    if (mode == "tokens") {
      this->printTokens(source, llvm::outs());
    } else {
      this->printTree(source, llvm::outs());
    }
  }

  return 0;
}

std::vector<SyntaxCommand::SourceText>
SyntaxCommand::readSources(llvm::ArrayRef<const char *> words) const {
  auto sources = std::vector<SourceText>();
  for (llvm::StringRef word : words) {
    if (!word.consume_front("@")) {
      this->addSource(word, sources);
      continue;
    }
    auto list = llvm::MemoryBuffer::getFile(word);
    if (!list) {
      continue;
    }
    auto lines = llvm::SmallVector<llvm::StringRef, 64>();
    (*list)->getBuffer().split(lines, '\n', -1, false);
    for (llvm::StringRef line : lines) {
      this->addSource(line.trim(), sources);
    }
  }

  return sources;
}

void SyntaxCommand::addSource(llvm::StringRef path, std::vector<SourceText> &sources) const {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (path.empty() || !buffer) {
    return;
  }
  std::string slashed = llvm::sys::path::convert_to_slash(path);
  bool isLibrary = llvm::StringRef(slashed).contains("/library/");
  sources.push_back({
      .path = path.str(),
      .text = (*buffer)->getBuffer().str(),
      .isLibrary = isLibrary,
  });
}

void SyntaxCommand::printTokens(const SourceText &source, llvm::raw_ostream &out) const {
  llvm::SourceMgr sourceMgr;
  Diagnostics diags(sourceMgr);
  Lexer lexer(source.text, diags);
  const char *begin = source.text.data();
  for (;;) {
    Token tok = lexer.next();
    out << static_cast<int>(tok.kind) << " " << (tok.text.data() - begin) << " " << tok.text.size()
        << " " << (tok.newlineBefore ? 1 : 0) << " " << (tok.hash ? 1 : 0) << "\n";
    if (tok.is(Tok::Eof)) {
      return;
    }
  }
}

void SyntaxCommand::printTree(const SourceText &source, llvm::raw_ostream &out) const {
  llvm::SourceMgr sourceMgr;
  Diagnostics diags(sourceMgr);
  AstContext ctx;
  Lexer lexer(source.text, diags);
  Parser parser(lexer, ctx, diags, source.isLibrary);
  SourceFile file = parser.parseSourceFile();
  AstDump dump(out, source.text.data());
  dump.dumpFile(file);
  auto taken = diags.take();
  if (taken.isOk()) {
    return;
  }
  for (const Diagnostic &found : taken.error()) {
    out << "!! " << (found.loc.getPointer() - source.text.data()) << "\n";
  }
}

int SyntaxCommand::timeSources(llvm::ArrayRef<SourceText> sources, int repeat) const {
  int64_t tokens = 0;
  int64_t nodes = 0;
  auto lexStart = std::chrono::steady_clock::now();
  for (int pass = 0; pass < repeat; ++pass) {
    tokens = this->lexSources(sources);
  }
  auto lexEnd = std::chrono::steady_clock::now();
  for (int pass = 0; pass < repeat; ++pass) {
    nodes = this->parseSources(sources);
  }
  auto parseEnd = std::chrono::steady_clock::now();
  double cloneMs = 0;
  for (int pass = 0; pass < repeat; ++pass) {
    cloneMs += this->cloneSources(sources);
  }
  auto lexMs = std::chrono::duration<double, std::milli>(lexEnd - lexStart).count() / repeat;
  auto parseMs = std::chrono::duration<double, std::milli>(parseEnd - lexEnd).count() / repeat;
  llvm::outs() << "files " << sources.size() << ", tokens " << tokens << ", nodes " << nodes
               << "\n";
  llvm::outs() << "lex " << llvm::format("%.2f", lexMs) << " ms, parse "
               << llvm::format("%.2f", parseMs) << " ms per pass, of it clone "
               << llvm::format("%.2f", cloneMs / repeat) << " ms\n";

  return 0;
}

int64_t SyntaxCommand::lexSources(llvm::ArrayRef<SourceText> sources) const {
  int64_t tokens = 0;
  for (const SourceText &source : sources) {
    llvm::SourceMgr sourceMgr;
    Diagnostics diags(sourceMgr);
    Lexer lexer(source.text, diags);
    while (!lexer.next().is(Tok::Eof)) {
      tokens += 1;
    }
  }

  return tokens;
}

double SyntaxCommand::cloneSources(llvm::ArrayRef<SourceText> sources) const {
  double total = 0;
  for (const SourceText &source : sources) {
    llvm::SourceMgr sourceMgr;
    Diagnostics diags(sourceMgr);
    AstContext ctx;
    Lexer lexer(source.text, diags);
    Parser parser(lexer, ctx, diags, source.isLibrary);
    SourceFile file = parser.parseSourceFile();
    auto start = std::chrono::steady_clock::now();
    AstCloner cloner(ctx);
    cloner.keepPristine(file.items);
    auto end = std::chrono::steady_clock::now();
    total += std::chrono::duration<double, std::milli>(end - start).count();
  }

  return total;
}

int SyntaxCommand::holdSources(llvm::ArrayRef<SourceText> sources, bool parse) const {
  auto contexts = std::vector<std::unique_ptr<AstContext>>();
  auto files = std::vector<SourceFile>();
  for (const SourceText &source : sources) {
    if (!parse) {
      continue;
    }
    llvm::SourceMgr sourceMgr;
    Diagnostics diags(sourceMgr);
    contexts.push_back(std::make_unique<AstContext>());
    Lexer lexer(source.text, diags);
    Parser parser(lexer, *contexts.back(), diags, source.isLibrary);
    files.push_back(parser.parseSourceFile());
  }
  size_t items = 0;
  for (const SourceFile &file : files) {
    items += file.items.size();
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));
  llvm::outs() << "files " << sources.size() << ", items " << items << "\n";

  return 0;
}

int64_t SyntaxCommand::parseSources(llvm::ArrayRef<SourceText> sources) const {
  int64_t nodes = 0;
  for (const SourceText &source : sources) {
    llvm::SourceMgr sourceMgr;
    Diagnostics diags(sourceMgr);
    AstContext ctx;
    Lexer lexer(source.text, diags);
    Parser parser(lexer, ctx, diags, source.isLibrary);
    SourceFile file = parser.parseSourceFile();
    nodes += static_cast<int64_t>(file.items.size());
  }

  return nodes;
}

}
