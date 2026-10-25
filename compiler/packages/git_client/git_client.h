#pragma once

#include <string>

namespace tz::pkg {

// The commit `ref` (a tag, a branch or a commit; "" is the default branch)
// names in the repository at `url`. The repository is mirrored into the cache
// and, unless it was just cloned, fetched first when `refresh` is set or the
// ref is unknown.
bool gitResolve(const std::string &url, const std::string &ref, bool refresh, std::string &commit,
                std::string &error);
// The files of `commit` in the cache, checked out if they are not there yet.
bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                 std::string &error);

} // namespace tz::pkg
