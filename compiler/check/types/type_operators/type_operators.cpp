#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/extends_relation/extends_relation.h"
#include "compiler/check/types/template_types/template_types.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

}

TypeOperators::TypeOperators(Checker &checker) : checker_(checker) {}

const Type *TypeOperators::keysOf(const Type *type) {
  type = type->nonOptional();
  if (type->isUnion()) {
    std::vector<const Type *> common;
    bool first = true;
    for (const Type *m : type->members()) {
      const Type *keys = keysOf(m);
      if (!keys) {
        return nullptr;
      }
      std::vector<const Type *> mine;
      for (const Type *k : membersOf(keys)) {
        if (first || llvm::is_contained(common, k)) {
          mine.push_back(k);
        }
      }
      common = std::move(mine);
      first = false;
    }
    return common.empty() ? Type::never() : Type::unionOf(common);
  }
  if (type->isRecord()) {
    return type->keyType();
  }
  if (!type->isObject() || type->isTuple() || type->isAnyObject()) {
    return nullptr;
  }
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls) {
    checker_.classDeclarer().completeClass(cls);
  }
  std::vector<const Type *> keys;
  for (const Field &f : type->fields()) {
    if (llvm::StringRef(f.name).starts_with("%")) {
      continue;
    }
    if (const ClassField *field = cls ? ClassBases::findField(cls, f.name) : nullptr;
        field && field->access != Access::Public) {
      continue;
    }
    keys.push_back(Type::stringLiteral(utf16(f.name)));
  }

  return keys.empty() ? Type::never() : Type::unionOf(keys);
}

const Type *TypeOperators::evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands) {
  TypeOp kind = op->typeOp();
  if (kind == TypeOp::Conditional) {
    return checker_.conditionalTypes().evalConditional(operands);
  }
  if (kind == TypeOp::Mapped) {
    const Type *key = operands[0], *keys = operands[1], *value = operands[2], *source = operands[3];
    const Type *rename = operands.size() > 4 ? operands[4] : nullptr;
    bool open = mentionsParam(keys) || (source && mentionsParam(source)) ||
                mentionsParamBesides(value, {key}) ||
                (rename && mentionsParamBesides(rename, {key}));
    if (open) {
      return Type::mapped(key, keys, value, op->mappedOptional(), op->mappedReadonly(), source,
                          rename);
    }
    if (keys->isNever()) {
      return Type::object({});
    }
    std::vector<Field> fields;
    for (const Type *name : membersOf(keys)) {
      if (!name->isStringLiteral()) {
        return Type::error();
      }
      std::string text = utf8(name->literalText());
      const Type *type = checker_.typeParams().substitute(value, {key}, {name});
      if (type->isError()) {
        return type;
      }
      const Field *from = source ? source->field(text) : nullptr;
      if (op->mappedOptional() > 0) {
        type = Type::optional(type);
      } else if (op->mappedOptional() < 0) {
        type = type->nonOptional();
      }
      bool readonly =
          op->mappedReadonly() > 0 || (op->mappedReadonly() == 0 && from && from->readonly);
      std::vector<std::string> names{text};
      if (rename) {
        const Type *renamed = checker_.typeParams().substitute(rename, {key}, {name});
        if (renamed->isError()) {
          return renamed;
        }
        names.clear();
        for (const Type *n : distributedMembers(renamed)) {
          if (!n->isStringLiteral()) {
            return Type::error();
          }
          names.push_back(utf8(n->literalText()));
        }
      }
      for (const std::string &field : names) {
        auto same = llvm::find_if(fields, [&](const Field &f) { return f.name == field; });
        if (same != fields.end()) {
          same->type = Type::unionOf({same->type, type});
        } else {
          fields.push_back({field, type, readonly});
        }
      }
    }
    return Type::object(std::move(fields));
  }
  if (llvm::any_of(operands, [](const Type *t) { return t && mentionsParam(t); })) {
    return Type::typeOp(kind, operands);
  }
  for (const Type *t : operands) {
    if (t && t->isError()) {
      return t;
    }
  }
  switch (kind) {
  case TypeOp::KeyOf: {
    const Type *keys = keysOf(operands[0]);
    return keys ? keys : Type::error();
  }
  case TypeOp::Index: {
    const Type *object = operands[0];
    if (operands[1]->isStringLiteral() && operands[1]->literalText() == u"length" &&
        (object->isTuple() || object->isArray())) {
      if (object->isTuple() && !object->tupleRest() &&
          object->tupleMin() == object->fields().size()) {
        return Type::numberLiteral(double(object->fields().size()), Type::intTy());
      }

      return Type::intTy();
    }
    const Type *member = checker_.argListChecker().memberType(operands[0], operands[1]);
    return member ? member : Type::error();
  }
  case TypeOp::Exclude:
  case TypeOp::Extract: {
    std::vector<const Type *> kept;
    for (const Type *m : distributedMembers(operands[0])) {
      if (checker_.extendsRelation().extendsType(m, operands[1]) == (kind == TypeOp::Extract)) {
        kept.push_back(m);
      }
    }
    return kept.empty() ? Type::never() : Type::unionOf(kept);
  }
  case TypeOp::Template:
    return checker_.templateTypes().evalTemplate(operands);
  case TypeOp::Spread: {
    std::vector<const Type *> elements;
    std::vector<bool> spread;
    for (size_t i = 0; i + 1 < operands.size(); i += 2) {
      spread.push_back(operands[i]->literalNumber() != 0);
      elements.push_back(operands[i + 1]);
    }
    std::string why;
    const Type *tuple = spreadTuple(elements, spread, why);
    return tuple ? tuple : Type::error();
  }
  case TypeOp::Uppercase:
  case TypeOp::Lowercase:
  case TypeOp::Capitalize:
  case TypeOp::Uncapitalize:
    return checker_.templateTypes().evalStringCase(kind, operands[0]);
  case TypeOp::NonNullable:
    return operands[0]->nonOptional();
  case TypeOp::ReturnType:
  case TypeOp::Parameters: {
    const Type *fn = operands[0]->nonOptional();
    if (!fn->isFunction()) {
      return Type::error();
    }
    if (kind == TypeOp::ReturnType) {
      return fn->ret();
    }
    std::vector<const Type *> params(fn->params().begin(),
                                     fn->params().end() - (fn->hasRest() ? 1 : 0));
    return Type::argumentList(params, std::min(fn->minArgs(), params.size()),
                              fn->hasRest() ? fn->params().back() : nullptr);
  }
  default:
    return Type::error();
  }
}

const Type *TypeOperators::resolveTypeOp(const TypeRef &ref) {
  switch (ref.kind) {
  case TypeRef::Kind::KeyOf: {
    const Type *type = checker_.typeResolver().resolveType(ref.args[0], false);
    if (type->isError()) {
      return type;
    }
    const Type *keys = evalTypeOp(Type::typeOp(TypeOp::KeyOf, {type}), {type});
    if (keys->isError()) {
      checker_.diags_.error(ref.loc, "'keyof' takes an object type, whose fields it names, not '" +
                                         type->name() + "'");
    }
    return keys;
  }
  case TypeRef::Kind::Indexed: {
    const Type *object = checker_.typeResolver().resolveType(ref.args[0], false);
    const Type *key = checker_.typeResolver().resolveType(ref.args[1], false);
    if (object->isError() || key->isError()) {
      return Type::error();
    }
    const Type *member = evalTypeOp(Type::indexedAccess(object, key), {object, key});
    if (member->isError()) {
      checker_.diags_.error(ref.loc,
                            "'" + object->name() + "' has no member '" + key->name() + "'");
    }
    return member;
  }
  case TypeRef::Kind::TypeOf: {
    llvm::StringRef name = ref.names.front();
    const Type *type = nullptr;
    Checker::ScopeEntry entry = checker_.topLevelChecker().lookupValue(name);
    if (VarSymbol *var = entry.var) {
      type = var->type;
    } else if (entry.cls && !entry.cls->isGeneric()) {
      type = checker_.classValueChecker().classValueType(entry.cls);
    } else if (entry.func && !entry.func->isGeneric()) {
      type = checker_.returnInferrer().functionType(entry.func);
    }
    for (Stmt *item : checker_.module_ ? checker_.module_->file.items : std::vector<Stmt *>()) {
      if (type) {
        break;
      }
      if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
        for (const VarDeclarator &d : decl->decls) {
          if (d.name == name && !type) {
            type = d.typeAnn ? checker_.typeResolver().resolveType(*d.typeAnn, false)
                   : d.init  ? GenericChecker::literalShape(d.init)
                             : nullptr;
          }
        }
      }
      if (auto *decl = dyn_cast<VarDeclStmt>(item); decl && !type) {
        for (const VarDeclarator &d : decl->decls) {
          if (auto *create = d.name == name ? llvm::dyn_cast_or_null<NewExpr>(d.init) : nullptr) {
            type = checker_.eventMaps().createdType(create);
          }
        }
      }
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (!fn || fn->name != name || !fn->typeParams.empty() || !fn->returnType) {
        continue;
      }
      std::vector<const Type *> params;
      size_t minArgs = 0;
      bool written = true;
      for (const Param &p : fn->params) {
        if (!p.type) {
          written = false;
          break;
        }
        const Type *t = checker_.typeResolver().resolveType(*p.type, false);
        params.push_back(p.optional || p.defaultValue ? Type::optional(t) : t);
        if (!p.optional && !p.defaultValue && !p.rest) {
          minArgs = params.size();
        }
      }
      if (written) {
        type = Type::function(params, minArgs, !fn->params.empty() && fn->params.back().rest,
                              checker_.typeResolver().resolveType(*fn->returnType, true));
      }
    }
    for (const ImportDecl &decl :
         checker_.module_ && !type ? checker_.module_->file.imports : std::vector<ImportDecl>()) {
      for (const ModuleName &imported : decl.names) {
        if (imported.as == name && decl.from && !type) {
          type = checker_.utilityTypes().importedValueType(decl.from, imported.name);
        }
      }
    }
    if (!type) {
      checker_.diags_.error(ref.loc, "'typeof " + name + "' needs the type of '" + name +
                                         "', which is not known here; declare it, e.g. 'const " +
                                         name + ": T = ...'");
      return Type::error();
    }
    for (size_t i = 1; i < ref.names.size(); ++i) {
      const Field *field = type->isObject() ? type->field(ref.names[i]) : nullptr;
      if (ClassDecl *cls = field ? nullptr : ClassDeclarer::classOf(type)) {
        checker_.classDeclarer().completeClass(cls);
        std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, ref.names[i], false);
        if (methods.size() == 1 && methods[0]->sym && methods[0]->typeParams.empty()) {
          type = checker_.returnInferrer().functionType(methods[0]->sym);
          continue;
        }
      }
      if (!field) {
        checker_.diags_.error(ref.loc,
                              "'" + type->name() + "' has no field '" + ref.names[i] + "'");
        return Type::error();
      }
      type = field->type;
    }
    return type;
  }
  case TypeRef::Kind::Mapped: {
    const Type *keys = checker_.typeResolver().resolveType(ref.args[0], false);
    if (keys->isError()) {
      return keys;
    }
    const Type *source = nullptr;
    if (ref.args[0].kind == TypeRef::Kind::KeyOf) {
      source = checker_.typeResolver().resolveType(ref.args[0].args[0], false);
    }
    for (const Type *k = keys; !source && k;) {
      if (k->typeOp() == TypeOp::KeyOf) {
        source = k->operands()[0];
      } else if (k->typeOp() == TypeOp::Exclude || k->typeOp() == TypeOp::Extract) {
        k = k->operands()[0];
      } else if (k->isParam() && k->constraint()) {
        k = k->constraint();
      } else {
        k = nullptr;
      }
    }
    Type *key = Type::param(ref.name);
    key->setConstraint(keys, {});
    checker_.typeScopes_.emplace_back();
    auto *alias = checker_.ctx_.make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
    alias->resolved = key;
    checker_.typeScopes_.back()[ref.name] = alias;
    const Type *value = checker_.typeResolver().resolveType(ref.args[1], false);
    const Type *rename = ref.args.size() > 2
                             ? checker_.conditionalTypes().resolveTypeArgument(ref.args[2])
                             : nullptr;
    checker_.typeScopes_.pop_back();
    if (value->isError()) {
      return value;
    }
    if (rename && rename->isError()) {
      return rename;
    }
    if (rename && !mentionsParam(rename)) {
      for (const Type *n : distributedMembers(rename)) {
        if (!n->isStringLiteral()) {
          checker_.diags_.error(ref.args[2].loc,
                                "a mapped type renames its keys to names of fields, string literal "
                                "types such as `get${Capitalize<K>}`, not '" +
                                    n->name() + "'");
          return Type::error();
        }
      }
    }
    for (const Type *k : mentionsParam(keys) ? llvm::ArrayRef<const Type *>() : membersOf(keys)) {
      if (!k->isStringLiteral() && !k->isNever()) {
        checker_.diags_.error(ref.args[0].loc,
                              "the keys of a mapped type are names of fields, string literal types "
                              "such as '\"id\" | \"name\"' or 'keyof T', not '" +
                                  k->name() + "'");
        return Type::error();
      }
    }
    const Type *mapped =
        Type::mapped(key, keys, value, ref.mappedOptional, ref.mappedReadonly, source, rename);
    return evalTypeOp(mapped, mapped->operands());
  }
  default:
    return nullptr;
  }
}

}
