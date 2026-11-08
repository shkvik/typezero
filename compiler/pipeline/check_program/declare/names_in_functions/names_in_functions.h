#pragma once

namespace tz {

class CheckState;
struct SourceFile;

class NamesInFunctions {
public:
  explicit NamesInFunctions(CheckState &owner);

  void rememberNames(SourceFile &program);

private:
  CheckState &state;
};

}
