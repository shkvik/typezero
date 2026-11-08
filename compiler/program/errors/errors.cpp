#include "compiler/program/errors/errors.h"

namespace tz {

void Errors::add(Diagnostic diagnostic) {
  if (diagnostic.kind == llvm::SourceMgr::DK_Error) {
    ++errorCount_;
  }
  this->list.push_back(std::move(diagnostic));
}

void Errors::add(const Errors &other) {
  for (const Diagnostic &diagnostic : other) {
    add(diagnostic);
  }
}

void Errors::add(const Warnings &warnings) {
  for (const Diagnostic &diagnostic : warnings) {
    add(diagnostic);
  }
}

bool Errors::isEmpty() const {
  return this->list.empty();
}

void Errors::tool(const llvm::Twine &message) {
  add({llvm::SourceMgr::DK_Error, Diagnostic::Form::Tool, llvm::SMLoc(), message.str()});
}

void Errors::raw(const llvm::Twine &text, llvm::SourceMgr::DiagKind kind) {
  add({kind, Diagnostic::Form::Raw, llvm::SMLoc(), text.str()});
}

}
