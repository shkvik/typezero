#pragma once

#include "llvm/Support/JSON.h"
#include <string>

namespace tz {

class TextDocument {
public:
  TextDocument(std::string uri, std::string path, std::string text);

  void apply(const llvm::json::Object &change);
  size_t offset(const llvm::json::Object *position) const;

  const std::string &uri() const { return uri_; }
  const std::string &path() const { return path_; }
  const std::string &key() const { return key_; }
  const std::string &text() const { return text_; }

  std::string entry;
  bool alone = false;

private:
  std::string uri_;
  std::string path_;
  std::string key_;
  std::string text_;
};

}
