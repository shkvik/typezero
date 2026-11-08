#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

bool operator==(const FieldPath &left, const FieldPath &right) {
  return left.var == right.var && left.names == right.names;
}

bool operator==(const EmitterKey &left, const EmitterKey &right) {
  return left.path == right.path && left.text == right.text && left.root == right.root;
}

}
