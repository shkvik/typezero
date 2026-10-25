#pragma once

#include <utility>

namespace tz {

template <class E> struct Err {
  E error;
};

template <class E> Err<E> err(E error) { return {std::move(error)}; }

} // namespace tz
