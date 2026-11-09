#pragma once

namespace tz {

class SemanticParts;
class ImportBinder;

class SemanticState;
struct SourceFile;

class NamesInFunctions {
public:
  NamesInFunctions(SemanticState &owner, SemanticParts &all);

  void rememberNames(SourceFile &program);

private:
  SemanticState &state;
  ImportBinder *importBinder = nullptr;
};

}
