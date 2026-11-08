#pragma once

#include "compiler/program/source_file/source_file.h"

namespace tz {

class Unused {
public:
  static void findUnused(SourceFile &program);
};

}
