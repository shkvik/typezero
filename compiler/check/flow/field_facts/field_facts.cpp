#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;

namespace tz {

FieldFacts::FieldFacts(Checker &owner)
    : checker(owner) {}

std::optional<Checker::FieldPath> FieldFacts::fieldPath(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(unnarrowed(expr));
  if (!member || member->optional || !member->object->type) {
    return std::nullopt;
  }
  const Type *holder = member->object->type;
  bool half = holder->isResult() && (member->name == "value" || member->name == "error");
  if (!(holder->isObject() || holder->isUnion() || holder->nonOptional()->isJson() || half)) {
    return std::nullopt;
  }
  const Expr *object = unnarrowed(member->object);
  Checker::FieldPath path;
  if (auto *name = dyn_cast<NameExpr>(object)) {
    if (!name->var()) {
      return std::nullopt;
    }
    path.var = name->var();
  } else if (std::optional<Checker::FieldPath> outer = this->fieldPath(object)) {
    path = std::move(*outer);
  } else {
    return std::nullopt;
  }
  path.names.push_back(member->name);

  return path;
}

bool FieldFacts::isFieldDefined(const Checker::FieldPath &path) const {
  return llvm::is_contained(this->checker.flow_.definedFields, path);
}

void FieldFacts::defineField(Checker::Flow &flow, const Checker::FieldPath &path) {
  if (!llvm::is_contained(flow.definedFields, path)) {
    flow.definedFields.push_back(path);
  }
}

void FieldFacts::fieldsAssigned(llvm::StringRef name) {
  llvm::erase_if(this->checker.flow_.definedFields, [&](const Checker::FieldPath &path) {
    return llvm::is_contained(path.names, name);
  });
  llvm::erase_if(this->checker.flow_.narrowedFields,
                 [&](const auto &entry) { return llvm::is_contained(entry.first.names, name); });
}

void FieldFacts::forgetFieldsOf(Checker::Flow &flow, const VarSymbol *var) {
  llvm::erase_if(flow.definedFields,
                 [&](const Checker::FieldPath &path) { return path.var == var; });
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first.var == var; });
}

Expr *FieldFacts::narrowedField(MemberExpr *member) {
  if (!this->checker.streamViews_.empty()) {
    if (std::optional<Checker::FieldPath> path = this->fieldPath(member)) {
      if (const Type *view =
              this->checker.setEncodingChecker().streamViewOf(*path, member->loc())) {
        return this->checker.typeRelations().convert(member, view);
      }
    }
  }
  if (this->checker.flow_.narrowedFields.empty()) {
    return member;
  }
  std::optional<Checker::FieldPath> path = this->fieldPath(member);
  if (!path) {
    return member;
  }
  for (const auto &[narrowed, type] : this->checker.flow_.narrowedFields) {
    if (narrowed == *path) {
      return this->checker.typeRelations().convert(member, type);
    }
  }

  return member;
}

}
