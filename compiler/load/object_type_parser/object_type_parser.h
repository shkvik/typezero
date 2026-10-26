#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class ObjectTypeParser {
public:
  explicit ObjectTypeParser(Parser &parser);

  bool mappedTypeAhead();
  TypeRef parseMappedType(TypeRef &object, int readonlyChange);
  TypeRef parseObjectType();
  TypeRef parseTupleType();
  bool parseFieldName(llvm::StringRef &name);

private:
  Parser &parser_;
};

}
