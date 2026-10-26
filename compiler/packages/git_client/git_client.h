#pragma once

#include <string>

namespace tz::pkg {

bool gitResolve(const std::string &url, const std::string &ref, bool refresh, std::string &commit,
                std::string &error);
bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                 std::string &error);

}
