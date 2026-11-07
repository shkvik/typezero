#include "compiler/pipeline/check_program/check_state/module_states/module_states_types.h"

namespace tz {

bool operator==(const ModuleBinding &left, const ModuleBinding &right) {
  return left.module == right.module && left.name == right.name;
}

}
