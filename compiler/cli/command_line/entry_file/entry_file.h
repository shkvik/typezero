#pragma once

namespace tz {

class Args;

class EntryFile {
public:
  int find(Args &args) const;

private:
  static int takeOutputDir(Args &args);
};

}
