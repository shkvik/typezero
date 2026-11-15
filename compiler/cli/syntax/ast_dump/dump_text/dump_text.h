#pragma once

#include "compiler/program/ast/ast.h"
#include <string>
#include <vector>

namespace tz {

class DumpText {
public:
  explicit DumpText(const char *begin);

  int64_t offset(llvm::SMLoc loc) const;
  std::string span(llvm::StringRef text) const;
  std::string quote(llvm::StringRef text) const;
  std::string units(const std::u16string &text) const;
  std::string bits(double value) const;
  std::string flags(const std::vector<bool> &values) const;
  std::string quoteAll(const std::vector<llvm::StringRef> &names) const;
  std::string typeKind(const TypeRef &ref) const;
  std::string typeExtras(const TypeRef &ref) const;
  std::string role(const FunctionDecl *fn) const;

private:
  const char *begin;
};

}
