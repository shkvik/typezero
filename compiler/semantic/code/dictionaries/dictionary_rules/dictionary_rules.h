#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class DictionaryRules {
public:
  using ResultKeyTypeIsAllowed = Result<const Type *, CodeDictionariesRegistryCheckKeyTypeError>;

  explicit DictionaryRules(SemanticState &owner);

  void failDictNew(NewExpr *expr);
  void finishDictNew(NewExpr *expr, const Type *type, std::vector<Expr *> args);


  ResultKeyTypeIsAllowed checkKeyType(const Type *type);
  const Type *wantedElement(const Type *type, bool isMap) const;

private:
  SemanticState &state;
};

}
