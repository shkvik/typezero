#pragma once

#include <utility>

template <class T> struct Ok {
  T value;
};

template <> struct Ok<void> {};

template <class T> Ok<T> ok(T value) {
  return {std::move(value)};
}

Ok<void> ok();
