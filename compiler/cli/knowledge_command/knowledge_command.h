#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class KnowledgeCommand {
public:
  KnowledgeCommand(const char *argv0, llvm::StringRef name);

  int run(llvm::ArrayRef<const char *> args) const;

private:
  bool learn(const std::string &entry, Knowledge &out) const;

  const char *argv0_;
  std::string name_;
};

}
