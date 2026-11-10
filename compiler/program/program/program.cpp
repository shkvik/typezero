#include "compiler/program/program/program.h"

namespace tz {

Program::Program()
    : diags(sourceMgr) {}

Program::~Program() = default;

}
