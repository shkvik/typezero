#pragma once

#include <utility>

template <class T> struct Ok {
  T value;
};

template <class T> Ok<T> ok(T value) {
  return {std::move(value)};
}
