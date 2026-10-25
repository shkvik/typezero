#pragma once

#include "compiler/result/err/err.h"
#include "compiler/result/ok/ok.h"
#include <optional>
#include <utility>

namespace tz {

template <class T, class E> class Result {
public:
  Result(Ok<T> ok) : value_(std::move(ok.value)) {}
  Result(Err<E> err) : error_(std::move(err.error)) {}

  bool isOk() const { return value_.has_value(); }
  bool isErr() const { return error_.has_value(); }
  T &value() { return *value_; }
  const E &error() const { return *error_; }

private:
  std::optional<T> value_;
  std::optional<E> error_;
};

} // namespace tz
