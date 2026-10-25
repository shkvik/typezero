#pragma once

#include <utility>

namespace tz {

template <class T> struct Ok {
  T value;
};

template <class T> Ok<T> ok(T value) { return {std::move(value)}; }

} // namespace tz
