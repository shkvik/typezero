#pragma once

#include "compiler/packages/port/field_value/field_value.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tz::pkg {

struct ClassMembers {
  std::vector<FieldValue> values;
  std::vector<size_t> constructors;
  size_t lastField = std::string::npos;
};

}
