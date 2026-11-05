#pragma once

#include <type_traits>
#include <utility>
#include <variant>

template <class... Alternatives> class Union {
  template <class Type, class... Types>
  static constexpr int countOf = (0 + ... + int(std::is_same_v<Type, Types>));

  template <auto Value, auto... Values>
  static constexpr int sameValues = (0 + ... + int(Value == Values));

  static_assert(((countOf<Alternatives, Alternatives...> == 1) && ...),
                "every alternative of a Union is a different type");

public:
  template <class Alternative>
    requires(countOf<Alternative, Alternatives...> == 1)
  Union(Alternative alternative)
      : current(std::move(alternative)) {}

  auto kind() const {
    static_assert(((sameValues<Alternatives::kind, Alternatives::kind...> == 1) && ...),
                  "every alternative has its own kind");

    return std::visit(
        [](const auto &alternative) { return std::decay_t<decltype(alternative)>::kind; },
        this->current);
  }

  template <class Alternative> bool is() const {
    return std::holds_alternative<Alternative>(this->current);
  }

  template <class Alternative> Alternative *as() {
    return std::get_if<Alternative>(&this->current);
  }

  template <class Alternative> const Alternative *as() const {
    return std::get_if<Alternative>(&this->current);
  }

private:
  std::variant<Alternatives...> current;
};
