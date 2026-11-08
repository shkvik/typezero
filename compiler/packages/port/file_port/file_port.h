#pragma once

#include "compiler/load/token/token.h"
#include "compiler/packages/port/edit/edit.h"
#include "compiler/packages/port/note/note.h"
#include "compiler/packages/port/port_context/port_context.h"
#include <string>
#include <vector>

namespace tz::pkg {

class FilePort {
public:
  FilePort(std::string file, std::string text, const PortContext &ctx, Counts &counts)
      : file_(std::move(file)),
        text_(std::move(text)),
        ctx(ctx),
        counts(counts) {}

  void run();
  const std::string &text() const { return text_; }
  std::vector<Note> &notes() { return notes_; }

private:
  static bool isOperandEnd(const Token &t);
  static std::string templateText(llvm::StringRef literal);
  void retokenize();
  size_t at(size_t i) const {
    return static_cast<size_t>(this->toks[i].text.data() - text_.data());
  }
  size_t endOf(size_t i) const {
    return at(i) + this->toks[i].text.size();
  }
  bool is(size_t i, Tok kind) const {
    return i < this->toks.size() && this->toks[i].kind == kind;
  }
  bool isName(size_t i, llvm::StringRef name) const {
    return is(i, Tok::Identifier) && this->toks[i].text == name;
  }
  bool afterDot(size_t i) const {
    return i > 0 && (is(i - 1, Tok::Dot) || is(i - 1, Tok::QuestionDot));
  }
  void add(size_t begin, size_t end, std::string text, const char *kind, std::string note = "") {
    this->edits.push_back({begin, end, std::move(text), kind, std::move(note)});
  }
  bool apply();
  void privateName(size_t i);

  void modules();
  void exportDefault(size_t i);
  void exportList(size_t open, size_t close);
  void importDecl(size_t i);
  void streamHooks();
  bool members(const std::string &name, size_t skipBegin, size_t skipEnd,
               std::vector<std::string> &items);
  void tokens();
  void concatenations();
  size_t operand(size_t i) const;
  size_t unary(size_t i) const;
  bool chainMayStart(size_t i) const;
  bool chainEnds(size_t i) const;
  void manual();
  void note(size_t i, const std::string &message) { notes_.push_back({at(i), message}); }

  std::string file_;
  std::string text_;
  const PortContext &ctx;
  Counts &counts;
  std::vector<Token> toks;
  std::vector<size_t> close;
  std::vector<Edit> edits;
  std::vector<Note> notes_;
  llvm::StringSet<> notedPackages;
};

}
