#include "compiler/packages/version/version.h"
#include "compiler/packages/partial/partial.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

Version::Version(uint64_t majorNumber, uint64_t minorNumber, uint64_t patchNumber,
                 std::vector<std::string> prerelease)
    : major(majorNumber),
      minor(minorNumber),
      patch(patchNumber),
      pre(std::move(prerelease)) {}

std::string Version::str() const {
  std::string text = std::to_string(this->major) + "." + std::to_string(this->minor) + "." +
                     std::to_string(this->patch);
  for (size_t i = 0; i < this->pre.size(); ++i) {
    text += (i ? "." : "-") + this->pre[i];
  }

  return text;
}

void Version::dropPrerelease() {
  this->pre.clear();
}

bool Version::isPrerelease() const {
  return !this->pre.empty();
}

uint64_t Version::getMajor() const {
  return this->major;
}

uint64_t Version::getMinor() const {
  return this->minor;
}

uint64_t Version::getPatch() const {
  return this->patch;
}

int Version::compare(const Version &other) const {
  if (this->major != other.major) {
    return this->major < other.major ? -1 : 1;
  }
  if (this->minor != other.minor) {
    return this->minor < other.minor ? -1 : 1;
  }
  if (this->patch != other.patch) {
    return this->patch < other.patch ? -1 : 1;
  }
  if (this->pre.empty() || other.pre.empty()) {
    if (this->pre.empty() == other.pre.empty()) {
      return 0;
    }

    return this->pre.empty() ? 1 : -1;
  }
  for (size_t i = 0; i < this->pre.size() && i < other.pre.size(); ++i) {
    int order = Version::comparePrerelease(this->pre[i], other.pre[i]);
    if (order != 0) {
      return order;
    }
  }
  if (this->pre.size() == other.pre.size()) {
    return 0;
  }

  return this->pre.size() < other.pre.size() ? -1 : 1;
}

bool Version::operator<(const Version &other) const {
  return this->compare(other) < 0;
}

bool Version::operator==(const Version &other) const {
  return this->compare(other) == 0;
}

bool Version::parse(llvm::StringRef text, Version &out) {
  Partial partial;
  bool parsed = PartialParser::parsePartial(text.trim(), partial);
  if (!parsed || partial.count != 3) {
    return false;
  }
  out = partial.version();

  return true;
}

int Version::comparePrerelease(const std::string &mine, const std::string &theirs) {
  if (mine == theirs) {
    return 0;
  }
  bool mineNumeric = Version::isNumeric(mine);
  bool theirsNumeric = Version::isNumeric(theirs);
  if (mineNumeric && theirsNumeric) {
    if (mine.size() != theirs.size()) {
      return mine.size() < theirs.size() ? -1 : 1;
    }

    return mine < theirs ? -1 : 1;
  }
  if (mineNumeric != theirsNumeric) {
    return mineNumeric ? -1 : 1;
  }

  return mine < theirs ? -1 : 1;
}

bool Version::isNumeric(llvm::StringRef id) {
  return !id.empty() && llvm::all_of(id, llvm::isDigit);
}

}
