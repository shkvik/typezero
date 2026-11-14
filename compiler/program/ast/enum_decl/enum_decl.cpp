#include "compiler/program/ast/enum_decl/enum_decl.h"
#include "compiler/program/types/types.h"

namespace tz {

const Type *EnumDecl::valuesType() const {
  if (!this->isString) {
    return Type::intTy();
  }
  auto values = std::vector<const Type *>();
  for (const EnumMember &member : this->members) {
    if (member.isString) {
      values.push_back(Type::stringLiteral(member.text));
    }
  }
  if (values.empty()) {
    return Type::string();
  }
  const auto *type = Type::unionOf(values);
  Type::nameUnion(type, this->name);

  return type;
}

}
