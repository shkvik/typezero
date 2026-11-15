#include "compiler/semantic/semantic_state/declaration/module_state_context/module_state_types.h"

namespace tz {

bool operator==(const ModuleBinding &left, const ModuleBinding &right) {
  return left.module == right.module && left.name == right.name;
}

}
