#pragma once

namespace tz {

class Type;

class ForeignOut {
public:
  bool isOut(const Type *type) const;
  const Type *getElement(const Type *type) const;
};

}
