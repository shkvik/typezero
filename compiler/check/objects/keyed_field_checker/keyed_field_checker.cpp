#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool isInherited(llvm::StringRef name) {
  static const char *const names[] = {"toString",      "valueOf",        "hasOwnProperty",
                                      "isPrototypeOf", "toLocaleString", "propertyIsEnumerable",
                                      "constructor",   "__proto__"};

  return llvm::is_contained(names, name);
}

std::string noField(const Type *type, llvm::StringRef name) {
  std::string message =
      ("property '" + name + "' does not exist on type '" + type->name() + "'").str();
  if (isInherited(name)) {
    message += "; objects inherit nothing from Object.prototype";
  }

  return message;
}

std::string toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

const char kRuntimeKeys[] =
    "keys computed when the program runs need a dictionary: declare it as one, e.g. "
    "'const d: Record<string, int> = {}'";

}

KeyedFieldChecker::KeyedFieldChecker(Checker &checker) : checker_(checker) {}

std::optional<std::string> KeyedFieldChecker::constantKey(const Expr *key) const {
  if (auto *lit = dyn_cast<StringLitExpr>(key)) {
    return toUtf8(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty()) {
    return toUtf8(tmpl->quasis[0]);
  }
  if (auto *name = dyn_cast<NameExpr>(key); name && name->var) {
    if (auto found = checker_.constKeys_.find(name->var); found != checker_.constKeys_.end()) {
      return found->second;
    }
  }

  return std::nullopt;
}

void KeyedFieldChecker::keyNames(const Expr *key,
                                 llvm::function_ref<void(llvm::StringRef)> each) const {
  if (auto *lit = dyn_cast<StringLitExpr>(key)) {
    each(toUtf8(lit->value));
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty()) {
    each(toUtf8(tmpl->quasis[0]));
  } else if (auto *number = dyn_cast<IntLitExpr>(key)) {
    each(std::to_string(number->value));
  } else if (auto *name = dyn_cast<NameExpr>(key)) {
    for (const std::string &text : checker_.file_->stringConstants.lookup(name->name)) {
      each(text);
    }
  } else if (auto *member = dyn_cast<MemberExpr>(key)) {
    if (auto *object = dyn_cast<NameExpr>(member->object)) {
      for (const std::string &text :
           checker_.file_->stringConstants.lookup((object->name + "." + member->name).str())) {
        each(text);
      }
    }
  }
}

const Type *KeyedFieldChecker::keyedFieldType(const Type *type, const Type *keyType) {
  if (type->isTuple() || !type->isObject()) {
    return nullptr;
  }
  const ClassDecl *cls = type->isClass() ? ClassDeclarer::classOf(type) : nullptr;
  if (type->isClass() && !cls) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  const Type *common = nullptr;
  for (const Type *n : names) {
    if (!n->isStringLiteral()) {
      return nullptr;
    }
    std::string text = toUtf8(n->literalText());
    const Field *f = type->field(text);
    if (cls) {
      const ClassField *field = ClassBases::findField(cls, text, nullptr);
      if (!field || field->isStatic || field->access != Access::Public) {
        return nullptr;
      }
    }
    if (!f || (common && f->type != common)) {
      return nullptr;
    }
    common = f->type;
  }

  return common;
}

Expr *KeyedFieldChecker::checkKeyedField(IndexExpr *index, bool write) {
  const Type *type = index->object->type;
  index->type = Type::error();
  std::string name;
  if (type->isTuple()) {
    index->index = checker_.exprChecker().checkExpr(index->index, Type::intTy());
    const Type *keyType = index->index->type;
    if (keyType->isError()) {
      return index;
    }
    std::optional<int64_t> position =
        keyType->isInteger() ? checker_.constantFolder().intConstant(index->index) : std::nullopt;
    size_t count = type->fields().size();
    if (!keyType->isInteger()) {
      checker_.diags_.error(index->index->loc(),
                            "a tuple is indexed by a number, found '" + keyType->name() + "'");
      return index;
    }
    if (!position) {
      checker_.diags_.error(
          index->loc(),
          "a tuple can be indexed only by a constant, such as '[0]': each element has "
          "its own type; or destructure it: 'const [a, b] = t'");
      return index;
    }
    if (*position < 0 || uint64_t(*position) >= count) {
      checker_.diags_.error(index->index->loc(),
                            "tuple '" + type->name() + "' has " + std::to_string(count) +
                                " element" + (count == 1 ? "" : "s") + "; there is no element " +
                                std::to_string(*position));
      return index;
    }
    name = std::to_string(*position);
  } else {
    index->index = checker_.exprChecker().checkExpr(index->index);
    const Type *keyType = index->index->type;
    if (keyType->isError()) {
      return index;
    }
    std::optional<std::string> key = constantKey(index->index);
    if (const Type *field = key ? nullptr : keyedFieldType(type, keyType)) {
      llvm::ArrayRef<const Type *> names =
          keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
      if (write) {
        if (field->isOptional() || field->isUnion() || field->isLiteral() ||
            llvm::any_of(names, [&](const Type *n) {
              return type->field(toUtf8(n->literalText()))->readonly;
            })) {
          checker_.diags_.error(index->loc(),
                                "fields of type '" + field->name() +
                                    "' are assigned by name, as in 'o.name = v'; through a "
                                    "key of literals only fields of a plain type are");
          return index;
        }
        for (const Type *n : names) {
          checker_.fieldFacts().fieldsAssigned(checker_.ctx_.keepName(toUtf8(n->literalText())));
        }
      }
      index->keyed = true;
      index->type = field;
      return index;
    }
    if (!key) {
      if (keyType->nonOptional()->isNumeric()) {
        checker_.diags_.error(index->loc(), "type '" + type->name() +
                                                "' has no numbered fields; read a field by its "
                                                "name, e.g. 'o.name'");
      } else if (keyType->isString()) {
        checker_.diags_.error(
            index->loc(),
            "type '" + type->name() +
                "' can be indexed only by a key known when compiling: a string literal, "
                "or a constant declared with one, such as 'const key = \"name\"'; " +
                kRuntimeKeys);
      } else {
        checker_.diags_.error(index->index->loc(),
                              "a key must be a string, found '" + keyType->name() + "'");
      }
      return index;
    }
    if (!type->isClass() && !type->field(*key)) {
      checker_.diags_.error(index->loc(), noField(type, *key));
      return index;
    }
    name = std::move(*key);
  }
  if (write && !checker_.assignedFields_.contains(name)) {
    checker_.diags_.error(index->index->loc(),
                          type->isTuple()
                              ? "an element of a tuple is assigned by its number: 't[0] = ...'"
                              : "a field assigned through a key in brackets must be "
                                "named by a string literal, or a constant declared with "
                                "one, such as 'const key = \"name\"'");
    return index;
  }
  index->field = checker_.ctx_.keepName(name);
  auto *member = checker_.ctx_.make<MemberExpr>(index->loc(), index->object, index->field);
  if (write) {
    return member;
  }

  return checker_.objectChecker().checkField(member);
}

}
