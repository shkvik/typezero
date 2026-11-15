#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

namespace union_rules {

template <auto... Kinds> constexpr bool areOwnKinds() {
  constexpr auto top = std::max({std::size_t(Kinds)...});
  auto seen = std::array<bool, top + 1>{};
  for (std::size_t kind : {std::size_t(Kinds)...}) {
    if (seen[kind]) {
      return false;
    }
    seen[kind] = true;
  }

  return true;
}

}

template <class... Alternatives> class Union {
  template <class Type>
  static constexpr auto matches =
      std::array<bool, sizeof...(Alternatives)>{std::is_same_v<Type, Alternatives>...};

  template <class Type>
  static constexpr int matchCount =
      int(std::count(matches<Type>.begin(), matches<Type>.end(), true));

  template <class Type>
  static constexpr int indexOf =
      int(std::find(matches<Type>.begin(), matches<Type>.end(), true) - matches<Type>.begin());

  static constexpr std::size_t storageSize = std::max({sizeof(Alternatives)...});
  static constexpr std::size_t storageAlignment = std::max({alignof(Alternatives)...});

public:
  template <class Alternative>
    requires(matchCount<Alternative> == 1)
  Union(Alternative alternative)
      : index(indexOf<Alternative>) {
    new (this->storage) Alternative(std::move(alternative));
  }

  Union(const Union &other);
  Union(Union &&other);
  Union &operator=(const Union &other);
  Union &operator=(Union &&other);
  ~Union();

  auto kind() const {
    static_assert(union_rules::areOwnKinds<Alternatives::kind...>(),
                  "every alternative has its own kind");
    static constexpr auto kinds = std::array{Alternatives::kind...};

    return kinds[this->index];
  }

  template <class Alternative> bool is() const {
    static_assert(matchCount<Alternative> == 1, "the type is one alternative of the Union");

    return this->index == indexOf<Alternative>;
  }

  template <class Alternative> Alternative *as() {
    if (!this->is<Alternative>()) {
      return nullptr;
    }

    return std::launder(reinterpret_cast<Alternative *>(this->storage));
  }

  template <class Alternative> const Alternative *as() const {
    if (!this->is<Alternative>()) {
      return nullptr;
    }

    return std::launder(reinterpret_cast<const Alternative *>(this->storage));
  }

private:
  void copyFrom(const Union &other);
  void moveFrom(Union &other);
  void destroy();

  template <class Alternative> void copyAs(const Union &other) {
    new (this->storage) Alternative(*other.as<Alternative>());
  }

  template <class Alternative> void moveAs(Union &other) {
    new (this->storage) Alternative(std::move(*other.as<Alternative>()));
  }

  template <class Alternative> void destroyAs() {
    this->as<Alternative>()->~Alternative();
  }

  alignas(storageAlignment) std::byte storage[storageSize];
  int index;
};

template <class... Alternatives>
Union<Alternatives...>::Union(const Union &other)
    : index(other.index) {
  this->copyFrom(other);
}

template <class... Alternatives>
Union<Alternatives...>::Union(Union &&other)
    : index(other.index) {
  this->moveFrom(other);
}

template <class... Alternatives>
Union<Alternatives...> &Union<Alternatives...>::operator=(const Union &other) {
  auto copied = Union(other);

  return *this = std::move(copied);
}

template <class... Alternatives>
Union<Alternatives...> &Union<Alternatives...>::operator=(Union &&other) {
  if (this == &other) {
    return *this;
  }
  auto moved = Union(std::move(other));
  this->destroy();
  this->index = moved.index;
  this->moveFrom(moved);

  return *this;
}

template <class... Alternatives> Union<Alternatives...>::~Union() {
  this->destroy();
}

template <class... Alternatives> void Union<Alternatives...>::copyFrom(const Union &other) {
  static constexpr void (Union::*copiers[])(const Union &) = {&Union::copyAs<Alternatives>...};
  (this->*copiers[other.index])(other);
}

template <class... Alternatives> void Union<Alternatives...>::moveFrom(Union &other) {
  static constexpr void (Union::*movers[])(Union &) = {&Union::moveAs<Alternatives>...};
  (this->*movers[other.index])(other);
}

template <class... Alternatives> void Union<Alternatives...>::destroy() {
  static constexpr void (Union::*destroyers[])() = {&Union::destroyAs<Alternatives>...};
  (this->*destroyers[this->index])();
}
