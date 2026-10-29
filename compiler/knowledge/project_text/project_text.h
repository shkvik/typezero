#pragma once

#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>
#include <vector>

namespace tz {

struct TextMention {
  std::string file;
  unsigned line = 0;
  std::string heading;
  std::string text;
};

class ProjectText {
public:
  explicit ProjectText(llvm::StringRef start);

  std::vector<TextMention> docs(llvm::StringRef name, llvm::StringRef qualified, size_t limit);
  std::vector<std::string> tests(llvm::StringRef name, size_t limit);
  const std::string &root() const { return root_; }

private:
  const std::vector<std::string> &lines(const std::string &file);
  void list();

  std::string root_;
  std::vector<std::string> docFiles_;
  std::vector<std::string> testFiles_;
  bool listed_ = false;
  std::map<std::string, std::vector<std::string>> cache_;
};

}
