#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ClassDeclarer;
class TemplateTypes;
class ReturnInferrer;

class ExtendsRelation {
public:
  ExtendsRelation(SemanticState &owner, SemanticParts &all);

  bool extendsType(const Type *type, const Type *pattern, InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, InferMatch *inference);

private:
  static std::vector<const Type *> distributed(const Type *type);

  SemanticState &state;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
