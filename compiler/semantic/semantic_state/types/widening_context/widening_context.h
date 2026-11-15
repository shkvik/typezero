#pragma once

#include <map>
#include <utility>

namespace tz {

class Type;

class WideningContext {
public:
  void rememberWidening(const Type *from, const Type *to, bool narrows);


  bool hasKnownWidening(const Type *from, const Type *to) const;
  bool isNarrowingWidening(const Type *from, const Type *to) const;

private:
  std::map<std::pair<const Type *, const Type *>, bool> narrowingWidenings;
};

}
