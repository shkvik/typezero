#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FieldFacts::FieldFacts(Checker &checker) : checker_(checker) {}

std::optional<Checker::FieldPath> FieldFacts::fieldPath(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(unnarrowed(expr));
  if (!member || member->optional || !member->object->type ||
      !(member->object->type->isObject() || member->object->type->isUnion() ||
        member->object->type->nonOptional()->isJson())) {
    return std::nullopt;
  }
  const Expr *object = unnarrowed(member->object);
  Checker::FieldPath path;
  if (auto *name = dyn_cast<NameExpr>(object)) {
    if (!name->var) {
      return std::nullopt;
    }
    path.var = name->var;
  } else if (std::optional<Checker::FieldPath> outer = fieldPath(object)) {
    path = std::move(*outer);
  } else {
    return std::nullopt;
  }
  path.names.push_back(member->name);

  return path;
}

bool FieldFacts::isFieldDefined(const Checker::FieldPath &path) const {
  return llvm::is_contained(checker_.flow_.definedFields, path);
}

void FieldFacts::defineField(Checker::Flow &flow, const Checker::FieldPath &path) {
  if (!llvm::is_contained(flow.definedFields, path)) {
    flow.definedFields.push_back(path);
  }
}

void FieldFacts::fieldsAssigned(llvm::StringRef name) {
  llvm::erase_if(checker_.flow_.definedFields, [&](const Checker::FieldPath &path) {
    return llvm::is_contained(path.names, name);
  });
  llvm::erase_if(checker_.flow_.narrowedFields,
                 [&](const auto &entry) { return llvm::is_contained(entry.first.names, name); });
}

void FieldFacts::forgetFieldsOf(Checker::Flow &flow, const VarSymbol *var) {
  llvm::erase_if(flow.definedFields,
                 [&](const Checker::FieldPath &path) { return path.var == var; });
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first.var == var; });
}

Expr *FieldFacts::narrowedField(MemberExpr *member) {
  if (!checker_.streamViews_.empty()) {
    if (std::optional<Checker::FieldPath> path = fieldPath(member)) {
      if (const Type *view = checker_.setEncodingChecker().streamViewOf(*path, member->loc())) {
        return checker_.typeRelations().convert(member, view);
      }
    }
  }
  if (checker_.flow_.narrowedFields.empty()) {
    return member;
  }
  std::optional<Checker::FieldPath> path = fieldPath(member);
  if (!path) {
    return member;
  }
  for (const auto &[narrowed, type] : checker_.flow_.narrowedFields) {
    if (narrowed == *path) {
      return checker_.typeRelations().convert(member, type);
    }
  }

  return member;
}

}
