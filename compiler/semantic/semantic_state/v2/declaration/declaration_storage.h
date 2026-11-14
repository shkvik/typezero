#pragma once

#include "compiler/semantic/semantic_state/v2/declaration/class_list_context/class_list_context.h"
#include "compiler/semantic/semantic_state/v2/declaration/declaration_order_context/declaration_order_context.h"
#include "compiler/semantic/semantic_state/v2/declaration/module_state_context/module_state_context.h"
#include "compiler/semantic/semantic_state/v2/declaration/name_mention_context/name_mention_context.h"
#include "compiler/semantic/semantic_state/v2/declaration/prelude_alias_context/prelude_alias_context.h"

namespace tz {

struct DeclarationStorage {
  ClassListContext classes;
  ModuleStateContext modules;
  NameMentionContext mentions;
  PreludeAliasContext prelude;
  DeclarationOrderContext order;
};

}
