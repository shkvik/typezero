#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;

namespace tz {

FieldFacts::FieldFacts(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeRelations = &all.getTypeRelations();
}

std::optional<FieldPath> FieldFacts::fieldPath(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(ObjectCheckHelpers::unnarrowed(expr));
  if (!member || member->optional || !member->object->type) {
    return std::nullopt;
  }
  const Type *holder = member->object->type;
  bool half = holder->isResult() && (member->name == "value" || member->name == "error");
  if (!(holder->isObject() || holder->isUnion() || holder->nonOptional()->isJson() || half)) {
    return std::nullopt;
  }
  const Expr *object = ObjectCheckHelpers::unnarrowed(member->object);
  FieldPath path;
  if (auto *name = dyn_cast<NameExpr>(object)) {
    if (!name->var()) {
      return std::nullopt;
    }
    path.var = name->var();
  } else if (std::optional<FieldPath> outer = this->fieldPath(object)) {
    path = std::move(*outer);
  } else {
    return std::nullopt;
  }
  path.names.push_back(member->name);

  return path;
}

bool FieldFacts::isFieldDefined(const FieldPath &path) const {
  return this->state.getFlowState().isFieldDefined(path);
}

void FieldFacts::defineField(Flow &flow, const FieldPath &path) {
  if (!llvm::is_contained(flow.definedFields, path)) {
    flow.definedFields.push_back(path);
  }
}

void FieldFacts::fieldsAssigned(llvm::StringRef name) {
  this->state.getFlowState().forgetFieldsNamed(name);
}

void FieldFacts::forgetFieldsOf(Flow &flow, const VarSymbol *var) {
  llvm::erase_if(flow.definedFields, [&](const FieldPath &path) { return path.var == var; });
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first.var == var; });
}

Expr *FieldFacts::narrowedField(MemberExpr *member) {
  if (this->state.getStreamStore().hasStreamViews()) {
    if (std::optional<FieldPath> path = this->fieldPath(member)) {
      if (const Type *view = this->state.getStreamStore().findStreamView(*path, member->loc())) {
        return this->typeRelations->convert(member, view);
      }
    }
  }
  if (!this->state.getFlowState().hasNarrowedFields()) {
    return member;
  }
  std::optional<FieldPath> path = this->fieldPath(member);
  if (!path) {
    return member;
  }
  if (const Type *type = this->state.getFlowState().findNarrowedField(*path)) {
    return this->typeRelations->convert(member, type);
  }

  return member;
}

}
