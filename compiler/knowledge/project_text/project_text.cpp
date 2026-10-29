#include "compiler/knowledge/project_text/project_text.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <algorithm>

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

bool isWord(char c) { return llvm::isAlnum(c) || c == '_' || c == '$'; }

bool hasWord(llvm::StringRef text, llvm::StringRef word) {
  if (word.empty()) {
    return false;
  }
  for (size_t at = text.find(word); at != llvm::StringRef::npos; at = text.find(word, at + 1)) {
    bool before = at == 0 || !isWord(text[at - 1]);
    size_t end = at + word.size();
    bool after = end >= text.size() || !isWord(text[end]);
    if (before && after) {
      return true;
    }
  }

  return false;
}

bool inCode(llvm::StringRef line, llvm::StringRef word) {
  size_t open = line.find('`');
  while (open != llvm::StringRef::npos) {
    size_t close = line.find('`', open + 1);
    if (close == llvm::StringRef::npos) {
      return false;
    }
    if (hasWord(line.slice(open + 1, close), word)) {
      return true;
    }
    open = line.find('`', close + 1);
  }

  return false;
}

void collect(llvm::StringRef dir, llvm::StringRef extension, std::vector<std::string> &out) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    if (path::extension(it->path()) == extension) {
      out.push_back(it->path());
    }
  }
  std::sort(out.begin(), out.end());
}

}

ProjectText::ProjectText(llvm::StringRef start) {
  llvm::SmallString<256> dir(start);
  for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
    llvm::SmallString<256> docs(dir);
    path::append(docs, "docs");
    llvm::SmallString<256> git(dir);
    path::append(git, ".git");
    if (fs::is_directory(docs) || fs::exists(git)) {
      root_ = std::string(dir);
      break;
    }
    llvm::StringRef parent = path::parent_path(dir);
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
}

void ProjectText::list() {
  if (listed_) {
    return;
  }
  llvm::SmallString<256> docs(root_);
  path::append(docs, "docs");
  collect(docs, ".md", docFiles_);
  llvm::SmallString<256> tests(root_);
  path::append(tests, "tests");
  collect(tests, ".tz", testFiles_);
  listed_ = true;
}

const std::vector<std::string> &ProjectText::lines(const std::string &file) {
  auto found = cache_.find(file);
  if (found != cache_.end()) {
    return found->second;
  }
  std::vector<std::string> &out = cache_[file];
  if (auto buffer = llvm::MemoryBuffer::getFile(file)) {
    llvm::SmallVector<llvm::StringRef, 0> split;
    (*buffer)->getBuffer().split(split, '\n');
    for (llvm::StringRef line : split) {
      out.push_back(line.rtrim("\r").str());
    }
  }

  return out;
}

std::vector<TextMention> ProjectText::docs(llvm::StringRef name, llvm::StringRef qualified,
                                           size_t limit) {
  std::vector<TextMention> found;
  if (root_.empty()) {
    return found;
  }
  list();
  std::vector<llvm::StringRef> words;
  if (qualified != name) {
    words.push_back(qualified);
  }
  words.push_back(name);
  for (llvm::StringRef word : words) {
    for (const std::string &file : docFiles_) {
      std::string heading;
      const std::vector<std::string> &text = lines(file);
      for (size_t i = 0; i < text.size() && found.size() < limit; ++i) {
        llvm::StringRef line = text[i];
        if (line.starts_with("#")) {
          heading = line.ltrim("# ").str();
          continue;
        }
        if (!inCode(line, word)) {
          continue;
        }
        bool known = llvm::any_of(found, [&](const TextMention &mention) {
          return mention.file == file && mention.heading == heading;
        });
        if (!known) {
          found.push_back({file, static_cast<unsigned>(i + 1), heading, line.trim().str()});
        }
      }
    }
    if (!found.empty()) {
      break;
    }
  }

  return found;
}

std::vector<std::string> ProjectText::tests(llvm::StringRef name, size_t limit) {
  std::vector<std::string> found;
  if (root_.empty()) {
    return found;
  }
  list();
  for (const std::string &file : testFiles_) {
    if (found.size() >= limit) {
      break;
    }
    auto buffer = llvm::MemoryBuffer::getFile(file);
    if (buffer && hasWord((*buffer)->getBuffer(), name)) {
      found.push_back(file);
    }
  }

  return found;
}

}
