#pragma once

#include "compiler/result/err/err.h"
#include "compiler/result/ok/ok.h"
#include <optional>
#include <utility>

template <class T, class E> class Result {
public:
  template <class U>
  Result(Ok<U> ok)
      : value_(std::move(ok.value)) {}
  template <class F>
  Result(Err<F> err)
      : error_(std::move(err.error)) {}

  bool isOk() const {
    return value_.has_value();
  }
  bool isErr() const {
    return error_.has_value();
  }
  T &value() {
    return *value_;
  }
  const E &error() const {
    return *error_;
  }

private:
  std::optional<T> value_;
  std::optional<E> error_;
};
