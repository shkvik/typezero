#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

bool ClassBases::isErrorClass(llvm::StringRef name) {
  static const char *const names[] = {"Error",       "TypeError",      "RangeError",
                                      "SyntaxError", "ReferenceError", "EvalError",
                                      "URIError",    "AggregateError"};

  return llvm::is_contained(names, name);
}

const char *ClassBases::accessName(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

ClassBases::ClassBases(Checker &owner)
    : checker(owner) {}

void ClassBases::addBase(const Type *type, const Type *base) {
  const_cast<Type *>(type)->addBase(base);
  bool emitted =
      !this->checker.trial_ && llvm::none_of(this->checker.functions_, [](const FunctionDecl *fn) {
        return !fn->typeParams.empty() && !fn->isInstance;
      });
  if (this->checker.file_ && emitted && !llvm::is_contained(this->checker.file_->subtypes, type)) {
    this->checker.file_->subtypes.push_back(type);
  }
}

void ClassBases::resolveClassBases(ClassDecl *cls) {
  cls->stage = ClassStage::RESOLVING_BASE;
  if (cls->base) {
    const TypeRef &ref = *cls->base;
    bool named = ref.kind() == TypeRefKind::NAME_REF || ref.kind() == TypeRefKind::GENERIC_REF;
    Stmt *found = named ? this->checker.typeDeclarer().lookupType(ref.name) : nullptr;
    auto *parent = llvm::dyn_cast_or_null<ClassDecl>(found);
    bool instance = false;
    if (parent && parent->library() && parent->name == "EventEmitter" &&
        ref.kind() == TypeRefKind::NAME_REF) {
      const Type *events = this->checker.eventMapInference().inferredEventMap(
          "class " + this->checker.eventMapInference().placeOf(cls->loc()) + " " + cls->name.str());
      ClassDecl *emitter = parent;
      parent = nullptr;
      if (events) {
        GenericClassInstantiator::ResultClassMade made =
            this->checker.genericClassInstantiator().instantiateClass(emitter, {events}, ref.loc);
        if (made.isErr() &&
            made.error().code !=
                GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
          llvm::SMLoc at =
              made.error().code ==
                      GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                  ? emitter->loc()
                  : ref.loc;
          this->checker.diags_.error(at, made.error().message);
        }
        parent = made.isOk() ? made.value() : nullptr;
      }
      instance = true;
      if (!events) {
        this->checker.diags_.error(ref.loc,
                                   "'EventEmitter' needs its event map here, e.g. 'extends "
                                   "EventEmitter<{ data: [chunk: string]; end: [] }>'");
      }
      if (!parent) {
        found = nullptr;
      }
    } else if (parent && (parent->isGeneric() || ref.kind() == TypeRefKind::GENERIC_REF)) {
      const Type *type = this->checker.typeResolver().resolveType(ref, false);
      parent = ClassDeclarer::classOf(type);
      instance = true;
      if (!parent) {
        found = nullptr;
      }
    }
    if (!named) {
      this->checker.diags_.error(ref.loc, "a class can extend only a class, by its name");
    } else if (instance && !parent) {
    } else if (parent && (parent == cls || parent->stage == ClassStage::RESOLVING_BASE)) {
      this->checker.diags_.error(
          ref.loc, "class '" + cls->name + "' cannot extend itself" +
                       (parent == cls ? std::string() : " through '" + parent->name.str() + "'"));
    } else if (this->checker.fsDeclarer().isRuntimeClass(parent)) {
      this->checker.diags_.error(ref.loc, "class '" + cls->name + "' cannot extend '" +
                                              parent->name +
                                              "': only the functions of node:fs make its objects");
    } else if (parent && parent->baseExpr && parent->stage == ClassStage::UNRESOLVED) {
      this->checker.diags_.error(ref.loc,
                                 "class '" + cls->name + "' cannot extend '" + parent->name +
                                     "', whose base is a value, made where its declaration runs; "
                                     "write it as an expression there: 'const " +
                                     cls->name + " = class extends " + parent->name + " { ... }'");
    } else if (parent) {
      std::vector<llvm::StringMap<Stmt *>> outer = this->checker.typeScopes_;
      this->checker.typeScopes_.resize(1);
      if (Module *home = this->checker.declModules_.lookup(parent);
          home && home != this->checker.module_ && this->checker.scopes_.size() == 1) {
        Module *here = this->checker.module_;
        this->checker.moduleDeclarer().useModule(home);
        this->checker.classDeclarer().resolveClass(parent);
        this->checker.moduleDeclarer().useModule(here);
      } else {
        this->checker.classDeclarer().resolveClass(parent);
      }
      this->checker.typeScopes_ = std::move(outer);
      cls->parent = parent;
      parent->children.push_back(cls);
      this->addBase(cls->type, parent->type);
    } else if (!found && ClassBases::isErrorClass(ref.name)) {
      this->checker.diags_.error(
          ref.loc, "there is no '" + ref.name + "' in tz: an error is a value; make '" + cls->name +
                       "' a type of object with a tag, e.g. 'type " + cls->name + " = { code: \"" +
                       cls->name + "\"; message: string }'");
    } else if (found) {
      std::string message =
          "'" + ref.name.str() + "' is not a class, and a class can extend only a class";
      if (isa<TypeAliasDecl>(found)) {
        message += "; to say that '" + cls->name.str() +
                   "' has what an interface lists, write "
                   "'implements " +
                   ref.name.str() + "'";
      }
      this->checker.diags_.error(ref.loc, message);
    } else {
      this->checker.diags_.error(ref.loc, "cannot find class '" + ref.name + "'");
    }
  }
  for (const TypeRef &ref : cls->interfaces) {
    const Type *type = this->checker.typeResolver().resolveType(ref, false);
    if (type->isError()) {
      continue;
    }
    if (type->isClass()) {
      this->checker.diags_.error(
          ref.loc, "'" + type->name() +
                       "' is a class, and a class implements only "
                       "interfaces" +
                       (cls->base ? std::string()
                                  : "; to take its fields and methods, write 'extends " +
                                        type->name().str() + "'"));
    } else if (!type->isObject() || type->isTuple() || type->isAnyObject() ||
               type->isIncomplete()) {
      this->checker.diags_.error(ref.loc, "'" + type->name() +
                                              "' is not an interface; a class implements object "
                                              "types, such as 'interface Named { name: string }'");
    } else if (!llvm::is_contained(cls->implemented, type)) {
      cls->implemented.push_back(type);
      this->addBase(cls->type, type);
    }
  }
  cls->stage = ClassStage::RESOLVED;

  for (ClassField &f : cls->fields) {
    if (f.isStatic()) {
      continue;
    }
    const ClassDecl *owner = nullptr;
    const ClassField *inherited =
        cls->parent ? ClassBases::findField(cls->parent, f.name, &owner) : nullptr;
    if (inherited && inherited->method()) {
      continue;
    }
    if (!inherited) {
      if (f.isOverride) {
        this->checker.diags_.error(
            f.loc, cls->parent
                       ? "'" + f.name + "' is marked 'override', but '" + cls->parent->name +
                             "' has no field '" + f.name + "'"
                       : "'override' says that '" + f.name + "' is a field of the class that '" +
                             cls->name + "' extends, and it extends none");
      }
      continue;
    }
    if (inherited->access == Access::Private) {
      this->checker.diags_.error(f.loc,
                                 "'" + f.name + "' is private in class '" + owner->name +
                                     "', so a class that extends it cannot declare it again");
      continue;
    }
    if (f.access > inherited->access) {
      this->checker.diags_.error(
          f.loc, "'" + f.name + "' is " + ClassBases::accessName(inherited->access) + " in '" +
                     owner->name + "', so it cannot be " + ClassBases::accessName(f.access) +
                     " in '" + cls->name + "'");
    }
    if (!f.init && !f.parameter()) {
      this->checker.diags_.error(
          f.loc, "'" + f.name + "' is a field of '" + owner->name +
                     "' already; declared again without a value, JavaScript would set "
                     "it to undefined after the constructor of '" +
                     owner->name + "' set it: drop the declaration, or give it a value");
    }
    f.redeclared = true;
  }
}

std::vector<Field> ClassBases::interfaceFields(TypeAliasDecl *alias,
                                               std::vector<const Type *> &bases,
                                               std::vector<Overload> &overloads) {
  std::vector<Field> fields;
  std::vector<std::string> origins;
  for (const TypeRef &ref : alias->bases) {
    Stmt *found = ref.kind() == TypeRefKind::NAME_REF
                      ? this->checker.typeDeclarer().lookupType(ref.name)
                      : nullptr;
    if (auto *other = llvm::dyn_cast_or_null<TypeAliasDecl>(found);
        other && (other == alias || other->resolving)) {
      this->checker.diags_.error(
          ref.loc, "interface '" + alias->name + "' cannot extend itself" +
                       (other == alias ? std::string() : " through '" + other->name.str() + "'"));
      continue;
    }
    const Type *base = this->checker.typeResolver().resolveType(ref, false);
    if (base->isError()) {
      continue;
    }
    if (base->isClass()) {
      this->checker.diags_.error(ref.loc, "'" + base->name() +
                                              "' is a class, and an interface extends only "
                                              "interfaces");
      continue;
    }
    if (!base->isObject() || base->isTuple() || base->isIncomplete() || base->isAnyObject()) {
      this->checker.diags_.error(ref.loc,
                                 "'" + base->name() +
                                     "' is not an interface; an interface extends "
                                     "object types, such as 'interface Named { name: string }'");
      continue;
    }
    if (llvm::is_contained(bases, base)) {
      continue;
    }
    bases.push_back(base);
    for (const Overload &o : base->overloads()) {
      auto it = llvm::find_if(overloads, [&](const Overload &p) { return p.name == o.name; });
      if (it == overloads.end()) {
        overloads.push_back(o);
      } else if (it->signatures != o.signatures) {
        this->checker.diags_.error(ref.loc, "interface '" + alias->name + "' gets method '" +
                                                o.name +
                                                "' from two interfaces with different signatures");
      }
    }
    std::string baseName = ref.name.str();
    for (const Field &f : base->fields()) {
      auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
      if (it == fields.end()) {
        fields.push_back(f);
        origins.push_back(baseName);
      } else if (it->type != f.type) {
        this->checker.diags_.error(ref.loc,
                                   "interface '" + alias->name + "' gets field '" + f.name +
                                       "' from '" + origins[size_t(it - fields.begin())] +
                                       "' as '" + it->type->name() + "' and from '" + baseName +
                                       "' as '" + f.type->name() + "'; a field has one type");
      } else {
        it->readonly = it->readonly && f.readonly;
      }
    }
  }
  const TypeRef &own = alias->type;
  std::vector<Overload> ownOverloads;
  std::vector<Field> ownFields = this->checker.typeResolver().resolveFields(own, &ownOverloads);
  for (const Overload &o : ownOverloads) {
    bool clash = llvm::any_of(fields, [&](const Field &f) { return f.name == o.name; }) ||
                 llvm::any_of(overloads, [&](const Overload &p) { return p.name == o.name; });
    if (clash) {
      this->checker.diags_.error(alias->loc(),
                                 "interface '" + alias->name + "' declares method '" + o.name +
                                     "' again; an interface cannot change the members of one "
                                     "it extends");
    } else {
      overloads.push_back(o);
    }
  }
  for (Field &f : ownFields) {
    if (llvm::any_of(overloads, [&](const Overload &o) { return o.name == f.name; })) {
      this->checker.diags_.error(alias->loc(),
                                 "interface '" + alias->name + "' declares '" + f.name +
                                     "' again; an interface cannot change the members of one "
                                     "it extends");
      continue;
    }
    llvm::SMLoc loc = alias->loc();
    for (size_t i = 0; i < own.names().size(); ++i) {
      if (own.names()[i] == f.name) {
        loc = own.locs()[i];
      }
    }
    auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
    if (it == fields.end()) {
      fields.push_back(f);
      origins.push_back(alias->name.str());
      continue;
    }
    const std::string &origin = origins[size_t(it - fields.begin())];
    if (it->type != f.type && !f.type->isError() && !it->type->isError() &&
        !WideWrites::sameReference(f.type, it->type)) {
      this->checker.diags_.error(
          loc, "field '" + f.name + "' of '" + alias->name + "' is '" + f.type->name() +
                   "', and '" + origin + "' declares it as '" + it->type->name() +
                   "'; an interface can narrow a field of one it extends only to objects "
                   "of a type down the chain of its type, such as 'Dog' for 'Animal', "
                   "which lie in memory as they do");
      continue;
    }
    this->checker.narrowedFields_ |= it->type != f.type || (f.readonly && !it->readonly);
    it->type = f.type;
    it->readonly = f.readonly;
  }

  return fields;
}

const ClassField *ClassBases::findField(const ClassDecl *cls, llvm::StringRef name,
                                        const ClassDecl **owner) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (const ClassField *f = c->field(name)) {
      if (owner) {
        *owner = c;
      }
      return f;
    }
  }

  return nullptr;
}

}
