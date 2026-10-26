#include "compiler/packages/locked/locked.h"

namespace tz::pkg {

std::string Locked::describe() const {
  if (!version.empty()) {
    return id + " " + version;
  }
  if (!commit.empty()) {
    return id + (ref.empty() ? std::string() : " " + ref) + " (" + commit.substr(0, 12) + ")";
  }

  return id;
}

}
