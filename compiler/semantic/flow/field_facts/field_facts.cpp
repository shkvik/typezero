#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
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
  std::vector<llvm::StringRef> names;
  const Expr *link = ObjectCheckHelpers::unnarrowed(expr);
  auto *member = dyn_cast<MemberExpr>(link);
  if (member == nullptr) {
    return std::nullopt;
  }
  while (member != nullptr) {
    if (!FieldFacts::holdsField(member)) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr || root->var() == nullptr) {
    return std::nullopt;
  }
  FieldPath path;
  path.var = root->var();
  for (llvm::StringRef name : llvm::reverse(names)) {
    path.names.push_back(name);
  }

  return path;
}

bool FieldFacts::holdsField(const MemberExpr *member) {
  if (member->optional || member->object->type == nullptr) {
    return false;
  }
  const Type *holder = member->object->type;
  bool value = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_VALUE);
  bool error = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_ERROR);
  bool resultHalf = holder->isResult() && (value || error);

  return holder->isObject() || holder->isUnion() || holder->nonOptional()->isJson() || resultHalf;
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
