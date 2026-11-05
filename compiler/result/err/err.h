#pragma once

#include <utility>

template <class E> struct Err {
  E error;
};

template <class E> Err<E> err(E error) {
  return {std::move(error)};
}
