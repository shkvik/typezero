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

} // namespace tz::pkg
