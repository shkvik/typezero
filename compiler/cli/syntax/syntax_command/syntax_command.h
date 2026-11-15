#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

namespace tz {

class SyntaxCommand {
public:
  int run(llvm::ArrayRef<const char *> words) const;

private:
  struct SourceText {
    std::string path;
    std::string text;
    bool isLibrary = false;
  };

  std::vector<SourceText> readSources(llvm::ArrayRef<const char *> words) const;
  void addSource(llvm::StringRef path, std::vector<SourceText> &sources) const;
  void printTokens(const SourceText &source, llvm::raw_ostream &out) const;
  void printTree(const SourceText &source, llvm::raw_ostream &out) const;
  int timeSources(llvm::ArrayRef<SourceText> sources, int repeat) const;
  int64_t lexSources(llvm::ArrayRef<SourceText> sources) const;
  int64_t parseSources(llvm::ArrayRef<SourceText> sources) const;
  double cloneSources(llvm::ArrayRef<SourceText> sources) const;
  int holdSources(llvm::ArrayRef<SourceText> sources, bool parse) const;
};

}
