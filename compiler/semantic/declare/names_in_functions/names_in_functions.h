#pragma once

namespace tz {

class SemanticState;
struct SourceFile;

class NamesInFunctions {
public:
  explicit NamesInFunctions(SemanticState &owner);

  void rememberNames(SourceFile &program);

private:
  SemanticState &state;
};

}
