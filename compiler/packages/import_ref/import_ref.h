#pragma once

#include <cstddef>
#include <string>

namespace tz::pkg {

// A module a file imports: the specifier and where its string literal is.
struct ImportRef {
  std::string specifier;
  size_t offset; // of the string literal
};

} // namespace tz::pkg
