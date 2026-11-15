#pragma once

#include "compiler/semantic/types/conditionals/types_conditionals_registry.h"
#include "compiler/semantic/types/intersections/types_intersections_registry.h"
#include "compiler/semantic/types/keys/types_keys_registry.h"
#include "compiler/semantic/types/mappings/types_mappings_registry.h"
#include "compiler/semantic/types/resolution/types_resolution_registry.h"
#include "compiler/semantic/types/text_patterns/types_text_patterns_registry.h"
#include "compiler/semantic/types/typeof/types_typeof_registry.h"

namespace tz {

struct TypeRegistries {
  TypesKeysRegistry keys;
  TypesTypeofRegistry typeOf;
  TypesMappingsRegistry mappings;
  TypesResolutionRegistry resolution;
  TypesConditionalsRegistry conditionals;
  TypesTextPatternsRegistry textPatterns;
  TypesIntersectionsRegistry intersections;
};

}
