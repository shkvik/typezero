#pragma once

#include "compiler/load/parser/parser.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"

namespace tz {

class DeclarationParser {
public:
  explicit DeclarationParser(Parser &parser);
  DeclarationFile parse();

private:
  void parseItem(DeclarationFile &file);
  void parseClass(DeclarationFile &file, bool isAbstract);
  void parseMember(DeclaredClass &cls);
  bool memberWordAhead() const;
  void skipItem();
  Parser &parser_;
};

}
