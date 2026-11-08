#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::dyn_cast;

namespace tz {

std::u16string TypeOperators::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

llvm::ArrayRef<const Type *> TypeOperators::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

TypeOperators::TypeOperators(SemanticState &owner)
    : state(owner) {}

const Type *TypeOperators::keysOf(const Type *type) {
  type = type->nonOptional();
  if (type->isUnion()) {
    std::vector<const Type *> common;
    bool first = true;
    for (const Type *m : type->members()) {
      const Type *keys = this->keysOf(m);
      if (!keys) {
        return nullptr;
      }
      std::vector<const Type *> mine;
      for (const Type *k : TypeOperators::membersOf(keys)) {
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
    ClassDeclarer(this->state).completeClass(cls);
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
    keys.push_back(Type::stringLiteral(TypeOperators::utf16(f.name)));
  }

  return keys.empty() ? Type::never() : Type::unionOf(keys);
}

const Type *TypeOperators::evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands) {
  TypeOp kind = op->typeOp();
  if (kind == TypeOp::Conditional) {
    return ConditionalTypes(this->state).evalConditional(operands);
  }
  if (kind == TypeOp::Mapped) {
    const Type *key = operands[0];
    const Type *keys = operands[1];
    const Type *value = operands[2];
    const Type *source = operands[3];
    const Type *rename = operands.size() > 4 ? operands[4] : nullptr;
    bool open = TypeQueries::mentionsParam(keys) ||
                (source && TypeQueries::mentionsParam(source)) ||
                ConditionalTypes::mentionsParamBesides(value, {key}) ||
                (rename && ConditionalTypes::mentionsParamBesides(rename, {key}));
    if (open) {
      return Type::mapped(key, keys, value, op->mappedOptional(), op->mappedReadonly(), source,
                          rename);
    }
    if (keys->isNever()) {
      return Type::object({});
    }
    std::vector<Field> fields;
    for (const Type *name : TypeOperators::membersOf(keys)) {
      if (!name->isStringLiteral()) {
        return Type::error();
      }
      std::string text = ToUtf8::toUtf8(name->literalText());
      const Type *type = TypeParams(this->state).substitute(value, {key}, {name});
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
        const Type *renamed = TypeParams(this->state).substitute(rename, {key}, {name});
        if (renamed->isError()) {
          return renamed;
        }
        names.clear();
        for (const Type *n : TypeopsCheckHelpers::distributedMembers(renamed)) {
          if (!n->isStringLiteral()) {
            return Type::error();
          }
          names.push_back(ToUtf8::toUtf8(n->literalText()));
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
  if (llvm::any_of(operands, [](const Type *t) { return t && TypeQueries::mentionsParam(t); })) {
    return Type::typeOp(kind, operands);
  }
  for (const Type *t : operands) {
    if (t && t->isError()) {
      return t;
    }
  }
  switch (kind) {
  case TypeOp::KeyOf: {
    const Type *keys = this->keysOf(operands[0]);
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
    const Type *member = ArgListChecker(this->state).memberType(operands[0], operands[1]);
    return member ? member : Type::error();
  }
  case TypeOp::Exclude:
  case TypeOp::Extract: {
    std::vector<const Type *> kept;
    for (const Type *m : TypeopsCheckHelpers::distributedMembers(operands[0])) {
      if (ExtendsRelation(this->state).extendsType(m, operands[1]) == (kind == TypeOp::Extract)) {
        kept.push_back(m);
      }
    }
    return kept.empty() ? Type::never() : Type::unionOf(kept);
  }
  case TypeOp::Template:
    return TemplateTypes(this->state).evalTemplate(operands);
  case TypeOp::Spread: {
    std::vector<const Type *> elements;
    std::vector<bool> spread;
    for (size_t i = 0; i + 1 < operands.size(); i += 2) {
      spread.push_back(operands[i]->literalNumber() != 0);
      elements.push_back(operands[i + 1]);
    }
    std::string why;
    const Type *tuple = ArgumentLists::spreadTuple(elements, spread, why);
    return tuple ? tuple : Type::error();
  }
  case TypeOp::Uppercase:
  case TypeOp::Lowercase:
  case TypeOp::Capitalize:
  case TypeOp::Uncapitalize:
    return TemplateTypes(this->state).evalStringCase(kind, operands[0]);
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

TypeOperators::ResultTypeOpResolved TypeOperators::resolveTypeOp(const TypeRef &ref) {
  switch (ref.kind()) {
    case TypeRefKind::KEY_OF_REF: {
      const Type *type = TypeResolver(this->state).resolveType(ref.args[0], false);
      if (type->isError()) {
        return ok(type);
      }
      const Type *keys = this->evalTypeOp(Type::typeOp(TypeOp::KeyOf, {type}), {type});
      if (keys->isError()) {
        std::string message = ErrorTextTypeOp::keyOf(type->name());

        return err(TypeOperatorsResolveTypeOpError{
            .code = TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE,
            .message = message,
        });
      }
      return ok(keys);
    }
    case TypeRefKind::INDEXED_REF: {
      const Type *object = TypeResolver(this->state).resolveType(ref.args[0], false);
      const Type *key = TypeResolver(this->state).resolveType(ref.args[1], false);
      if (object->isError() || key->isError()) {
        return ok(Type::error());
      }
      const Type *member = this->evalTypeOp(Type::indexedAccess(object, key), {object, key});
      if (member->isError()) {
        std::string message = ErrorTextTypeOp::noMember(object->name(), key->name());

        return err(TypeOperatorsResolveTypeOpError{
            .code = TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_MEMBER,
            .message = message,
        });
      }
      return ok(member);
    }
    case TypeRefKind::TYPE_OF_REF: {
      llvm::StringRef name = ref.names().front();
      const Type *type = nullptr;
      ScopeEntry entry = TopLevelChecker(this->state).lookupValue(name);
      if (VarSymbol *var = entry.var()) {
        type = var->type;
      } else if (entry.cls() && !entry.cls()->isGeneric()) {
        type = ClassValueChecker(this->state).classValueType(entry.cls());
      } else if (entry.func()) {
        type = UtilityTypes(this->state).functionValueType(entry.func());
      }
      for (Stmt *item : this->state.getModuleStates().getCurrentModule()
                            ? this->state.getModuleStates().getCurrentModule()->file.items
                            : std::vector<Stmt *>()) {
        if (type) {
          break;
        }
        if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
          for (const VarDeclarator &d : decl->decls) {
            if (d.name == name && !type) {
              type = d.typeAnn ? TypeResolver(this->state).resolveType(*d.typeAnn, false)
                     : d.init  ? GenericChecker::literalShape(d.init)
                               : nullptr;
            }
          }
        }
        if (auto *decl = dyn_cast<VarDeclStmt>(item); decl && !type) {
          for (const VarDeclarator &d : decl->decls) {
            if (auto *create = d.name == name ? llvm::dyn_cast_or_null<NewExpr>(d.init) : nullptr) {
              type = EventMaps(this->state).createdType(create);
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
          const Type *t = TypeResolver(this->state).resolveType(*p.type, false);
          params.push_back(p.optional() || p.defaultValue() ? Type::optional(t) : t);
          if (!p.optional() && !p.defaultValue() && !p.rest()) {
            minArgs = params.size();
          }
        }
        if (written) {
          type = Type::function(params, minArgs, !fn->params.empty() && fn->params.back().rest(),
                                TypeResolver(this->state).resolveType(*fn->returnType, true));
        }
      }
      for (const ImportDecl &decl :
           this->state.getModuleStates().getCurrentModule() && !type
               ? this->state.getModuleStates().getCurrentModule()->file.imports
               : std::vector<ImportDecl>()) {
        for (const ModuleName &imported : decl.names) {
          if (imported.as == name && decl.from && !type) {
            type = UtilityTypes(this->state).importedValueType(decl.from, imported.name);
          }
        }
      }
      if (!type) {
        std::string message = ErrorTextTypeOp::typeOf(name);

        return err(TypeOperatorsResolveTypeOpError{
            .code = TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN,
            .message = message,
        });
      }
      for (size_t i = 1; i < ref.names().size(); ++i) {
        const Field *field = type->isObject() ? type->field(ref.names()[i]) : nullptr;
        if (ClassDecl *cls = field ? nullptr : ClassDeclarer::classOf(type)) {
          ClassDeclarer(this->state).completeClass(cls);
          std::vector<FunctionDecl *> methods =
              ClassDeclarer::methodsNamed(cls, ref.names()[i], false);
          if (methods.size() == 1 && methods[0]->sym && methods[0]->typeParams.empty()) {
            type = ReturnInferrer(this->state).functionType(methods[0]->sym);
            continue;
          }
        }
        if (!field) {
          std::string message = ErrorTextTypeOp::noField(type->name(), ref.names()[i]);

          return err(TypeOperatorsResolveTypeOpError{
              .code = TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD,
              .message = message,
          });
        }
        type = field->type;
      }
      return ok(type);
    }
    case TypeRefKind::MAPPED_REF: {
      const Type *keys = TypeResolver(this->state).resolveType(ref.args[0], false);
      if (keys->isError()) {
        return ok(keys);
      }
      const Type *source = nullptr;
      if (ref.args[0].kind() == TypeRefKind::KEY_OF_REF) {
        source = TypeResolver(this->state).resolveType(ref.args[0].args[0], false);
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
      this->state.getScopeStack().openTypeScope();
      auto *alias = this->state.getAst().make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
      alias->resolved = key;
      this->state.getScopeStack().setType(ref.name, alias);
      const Type *value = TypeResolver(this->state).resolveType(ref.args[1], false);
      const Type *rename = ref.args.size() > 2
                               ? ConditionalTypes(this->state).resolveTypeArgument(ref.args[2])
                               : nullptr;
      this->state.getScopeStack().closeTypeScope();
      if (value->isError()) {
        return ok(value);
      }
      if (rename && rename->isError()) {
        return ok(rename);
      }
      if (rename && !TypeQueries::mentionsParam(rename)) {
        for (const Type *n : TypeopsCheckHelpers::distributedMembers(rename)) {
          if (!n->isStringLiteral()) {
            std::string message = ErrorTextTypeOp::rename(n->name());

            return err(TypeOperatorsResolveTypeOpError{
                .code = TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING,
                .message = message,
            });
          }
        }
      }
      for (const Type *k : TypeQueries::mentionsParam(keys) ? llvm::ArrayRef<const Type *>()
                                                            : TypeOperators::membersOf(keys)) {
        if (!k->isStringLiteral() && !k->isNever()) {
          std::string message = ErrorTextTypeOp::keys(k->name());

          return err(TypeOperatorsResolveTypeOpError{
              .code = TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS,
              .message = message,
          });
        }
      }
      const Type *mapped = Type::mapped(key, keys, value, ref.mappedOptional(),
                                        ref.mappedReadonly(), source, rename);
      const Type *evaluated = this->evalTypeOp(mapped, mapped->operands());
      return ok(evaluated);
    }
    default:
      return ok(nullptr);
  }
}

}
