#include "compiler/check/declarations/declaration_checker/declaration_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

const char *DeclarationChecker::accessWord(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

DeclarationChecker::DeclarationChecker(Checker &owner)
    : checker(owner) {}

std::string DeclarationChecker::fileName() const {
  return llvm::sys::path::filename(this->checkedModule->path).str();
}

const Type *DeclarationChecker::resolve(const std::optional<TypeRef> &ref, bool allowVoid) {
  if (!ref) {
    return nullptr;
  }

  return this->checker.typeResolver().resolveType(*ref, allowVoid);
}

void DeclarationChecker::mismatch(llvm::SMLoc loc, const std::string &what, const Type *here,
                                  const Type *there) {
  this->checker.diags_.error(loc, what + " is '" + here->name().str() + "' here, and '" +
                                      there->name().str() + "' in '" + this->fileName() + "'");
}

void DeclarationChecker::check(Module *module) {
  this->checkedModule = module;
  Module *outer = this->checker.module_;
  this->checker.moduleDeclarer().useModule(module);
  this->exportedNames.clear();
  for (const ExportDecl &decl : module->file.exports) {
    const LocalExport *local = decl.form.as<LocalExport>();
    if (!local) {
      continue;
    }
    for (const ModuleName &name : local->names) {
      this->exportedNames[name.as.empty() ? name.name : name.as] = {name.name, name.loc};
    }
  }
  llvm::StringSet<> declared;
  const DeclarationFile &file = *module->declaration;
  auto lookup = [&](llvm::StringRef name, llvm::SMLoc loc) -> const Exported * {
    declared.insert(name);
    auto found = this->exportedNames.find(name);
    if (found == this->exportedNames.end()) {
      this->checker.diags_.error(loc, "'" + name + "' is declared here, but '" + this->fileName() +
                                          "' does not export it");
      return nullptr;
    }

    return &found->second;
  };
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      if (const Exported *exported = lookup(alias->name, alias->loc())) {
        this->checkAlias(alias, *exported);
      }
    } else if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (const Exported *exported = lookup(fn->name, fn->loc())) {
        this->checkFunction(fn, *exported);
      }
    }
  }
  for (const DeclaredClass &cls : file.classes) {
    if (const Exported *exported = lookup(cls.name, cls.loc)) {
      this->checkClass(cls, *exported);
    }
  }
  std::string declarations = llvm::sys::path::filename(file.path).str();
  for (const auto &entry : this->exportedNames) {
    if (!declared.contains(entry.getKey())) {
      this->checker.diags_.error(
          entry.second.loc, "'" + entry.getKey() + "' is exported, but '" + declarations +
                                "' does not declare it: a declaration file declares all that its "
                                "module exports");
    }
  }
  if (outer) {
    this->checker.moduleDeclarer().useModule(outer);
  }
}

void DeclarationChecker::checkAlias(TypeAliasDecl *declared, const Exported &exported) {
  auto *impl =
      dyn_cast_or_null<TypeAliasDecl>(this->checker.typeDeclarer().lookupType(exported.local));
  if (!impl) {
    this->checker.diags_.error(declared->loc(), "'" + declared->name + "' is not a type in '" +
                                                    this->fileName() + "'");
    return;
  }
  if (declared->typeParams.size() != impl->typeParams.size()) {
    this->checker.diags_.error(declared->loc(), "'" + declared->name + "' takes " +
                                                    std::to_string(declared->typeParams.size()) +
                                                    " type parameters here, and " +
                                                    std::to_string(impl->typeParams.size()) +
                                                    " in '" + this->fileName() + "'");
    return;
  }
  if (!impl->typeParams.empty()) {
    return;
  }
  const Type *there = this->checker.typeDeclarer().resolveAlias(impl);
  const Type *here = this->checker.typeDeclarer().resolveAlias(declared);
  if (!here || !there || here == there || here->isError() || there->isError()) {
    return;
  }
  std::string what = "'" + declared->name.str() + "'";
  if (here->isObject() && there->isObject() && !here->isClass() && !there->isClass()) {
    for (const Field &mine : here->fields()) {
      const Field *theirs = there->field(mine.name);
      if (!theirs) {
        this->checker.diags_.error(declared->loc(), what + " has field '" + mine.name +
                                                        "' here, and not in '" + this->fileName() +
                                                        "'");
        return;
      }
      if (mine.type != theirs->type) {
        this->mismatch(declared->loc(), "field '" + mine.name + "' of " + what, mine.type,
                       theirs->type);
        return;
      }
      if (mine.readonly != theirs->readonly) {
        this->checker.diags_.error(declared->loc(),
                                   "field '" + mine.name + "' of " + what + " is " +
                                       (theirs->readonly ? "readonly" : "not readonly") + " in '" +
                                       this->fileName() + "'");
        return;
      }
    }
    for (const Field &theirs : there->fields()) {
      if (!here->field(theirs.name)) {
        this->checker.diags_.error(declared->loc(), what + " has field '" + theirs.name + "' in '" +
                                                        this->fileName() +
                                                        "', which is not declared here");
        return;
      }
    }
  }
  this->mismatch(declared->loc(), what, here, there);
}

bool DeclarationChecker::sameParams(const std::vector<Param> &declared, const FunctionDecl *fn,
                                    llvm::SMLoc loc, const std::string &what) {
  if (declared.size() != fn->params.size()) {
    this->checker.diags_.error(
        loc, what + " takes " + std::to_string(declared.size()) + " parameters here, and " +
                 std::to_string(fn->params.size()) + " in '" + this->fileName() + "'");
    return false;
  }
  for (size_t i = 0; i < declared.size(); ++i) {
    const Param &mine = declared[i];
    const Param &theirs = fn->params[i];
    std::string param = "parameter '" + mine.name.str() + "' of " + what;
    if (mine.optional() != (theirs.optional() || theirs.defaultValue()) ||
        mine.rest() != theirs.rest()) {
      this->checker.diags_.error(mine.loc, param + " is " +
                                               (mine.optional() ? "optional" : "required") +
                                               " here, and not in '" + this->fileName() + "'");
      return false;
    }
    const Type *here = this->resolve(mine.type, false);
    const Type *there = theirs.declaredType;
    if (!here || !there || here->isError() || there->isError()) {
      continue;
    }
    if (here != there && Type::optional(here) != there) {
      this->mismatch(mine.loc, "the type of " + param, here, there);
      return false;
    }
  }

  return true;
}

void DeclarationChecker::checkFunction(FunctionDecl *declared, const Exported &exported) {
  ScopeEntry entry = this->checker.topLevelChecker().lookupValue(exported.local);
  if (!entry.func() || !entry.func()->decl) {
    this->checker.diags_.error(declared->loc(), "'" + declared->name + "' is not a function in '" +
                                                    this->fileName() + "'");
    return;
  }
  const FunctionDecl *fn = entry.func()->decl;
  std::string what = "'" + declared->name.str() + "'";
  if (!declared->typeParams.empty() || !fn->typeParams.empty()) {
    if (declared->typeParams.size() != fn->typeParams.size()) {
      this->checker.diags_.error(declared->loc(),
                                 what + " takes another number of type parameters in '" +
                                     this->fileName() + "'");
    }
    return;
  }
  this->checker.returnInferrer().resolveParams(entry.func());
  if (!this->sameParams(declared->params, fn, declared->loc(), what)) {
    return;
  }
  if (!declared->returnType) {
    this->checker.diags_.error(declared->loc(), "write what " + what +
                                                    " gives: ': T' after its "
                                                    "parameters");
    return;
  }
  const Type *here = this->resolve(declared->returnType, true);
  const Type *there = this->checker.returnInferrer().returnType(entry.func());
  if (here && there && here != there && !here->isError() && !there->isError()) {
    this->mismatch(declared->loc(), "what " + what + " gives", here, there);
  }
}

bool DeclarationChecker::mentionsHidden(const FunctionDecl *fn) const {
  for (const Param &param : fn->params) {
    const Type *type = param.declaredType ? param.declaredType->nonOptional() : nullptr;
    const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
    if (!cls) {
      continue;
    }
    bool shown = false;
    for (const auto &entry : this->exportedNames) {
      if (entry.second.local == cls->name) {
        shown = true;
      }
    }
    if (!shown && this->checker.declModules_.lookup(cls) == this->checkedModule) {
      return true;
    }
  }

  return false;
}

void DeclarationChecker::checkField(const DeclaredMember &member, const ClassField &field,
                                    const ClassDecl *cls) {
  std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
  const DeclaredField *declared = member.form.as<DeclaredField>();
  if (!declared) {
    this->checker.diags_.error(member.loc, what + " is a field in '" + this->fileName() + "'");
    return;
  }
  if (declared->readonly != field.readonly) {
    this->checker.diags_.error(member.loc,
                               what + (field.readonly ? " is readonly" : " is not readonly") +
                                   " in '" + this->fileName() + "'");
  }
  if (declared->optional != field.optional()) {
    this->checker.diags_.error(member.loc,
                               what + (field.optional() ? " is optional" : " is not optional") +
                                   " in '" + this->fileName() + "'");
  }
  const Type *here = this->resolve(member.type, false);
  const Type *there = field.resolved;
  if (here && there && !here->isError() && !there->isError() && here != there &&
      Type::optional(here) != there) {
    this->mismatch(member.loc, "the type of " + what, here, there);
  }
}

void DeclarationChecker::checkMethod(const DeclaredMember &member, const FunctionDecl *fn,
                                     const ClassDecl *cls) {
  std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
  const DeclaredSignature *signature = nullptr;
  switch (member.form.kind()) {
    case DeclaredMemberKind::DECLARED_FIELD:
      break;
    case DeclaredMemberKind::DECLARED_METHOD:
      signature = &member.form.as<DeclaredMethod>()->signature;
      break;
    case DeclaredMemberKind::DECLARED_CONSTRUCTOR:
      signature = &member.form.as<DeclaredConstructor>()->signature;
      break;
  }
  if (!signature) {
    this->checker.diags_.error(member.loc, what + " is a method in '" + this->fileName() + "'");
    return;
  }
  if (!signature->typeParams.empty() || !fn->typeParams.empty()) {
    if (signature->typeParams.size() != fn->typeParams.size()) {
      this->checker.diags_.error(member.loc, what +
                                                 " takes another number of type parameters in '" +
                                                 this->fileName() + "'");
    }
    return;
  }
  this->checker.returnInferrer().resolveParams(fn->sym);
  bool constructor = member.form.is<DeclaredConstructor>();
  if (!this->sameParams(signature->params, fn, member.loc, what) || constructor) {
    return;
  }
  if (!member.type) {
    this->checker.diags_.error(member.loc, "write what " + what +
                                               " gives: ': T' after its "
                                               "parameters");
    return;
  }
  const Type *here = this->resolve(member.type, true);
  const Type *there = this->checker.returnInferrer().returnType(fn->sym);
  if (here && there && here != there && !here->isError() && !there->isError()) {
    this->mismatch(member.loc, "what " + what + " gives", here, there);
  }
}

void DeclarationChecker::checkClass(const DeclaredClass &declared, const Exported &exported) {
  auto *cls = dyn_cast_or_null<ClassDecl>(this->checker.typeDeclarer().lookupType(exported.local));
  if (!cls) {
    this->checker.diags_.error(declared.loc, "'" + declared.name + "' is not a class in '" +
                                                 this->fileName() + "'");
    return;
  }
  if (declared.typeParams.size() != cls->typeParams.size()) {
    this->checker.diags_.error(declared.loc, "'" + declared.name +
                                                 "' takes another number of type parameters in '" +
                                                 this->fileName() + "'");
    return;
  }
  if (!cls->typeParams.empty()) {
    return;
  }
  this->checker.classDeclarer().completeClass(cls);
  llvm::StringSet<> seen;
  for (const DeclaredMember &member : declared.members) {
    std::string key = (member.isStatic ? "static " : "") + member.name.str();
    seen.insert(key);
    std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
    if (!member.accessWritten) {
      this->checker.diags_.error(member.loc,
                                 "write the access of " + what + ": public, protected or private");
    }
    if (member.form.is<DeclaredConstructor>()) {
      const FunctionDecl *ctor = nullptr;
      for (const FunctionDecl *c : cls->constructors) {
        if (c->access() != Access::Private) {
          ctor = c;
        }
      }
      if (!ctor) {
        this->checker.diags_.error(member.loc, "'" + cls->name +
                                                   "' has no constructor that programs "
                                                   "can call in '" +
                                                   this->fileName() + "'");
        continue;
      }
      if (member.access != ctor->access()) {
        this->checker.diags_.error(member.loc, "the constructor of '" + cls->name + "' is " +
                                                   DeclarationChecker::accessWord(ctor->access()) +
                                                   " in '" + this->fileName() + "'");
      }
      this->checkMethod(member, ctor, cls);
      continue;
    }
    const ClassField *field = nullptr;
    for (const ClassField &f : cls->fields) {
      if (f.name == member.name && f.isStatic() == member.isStatic && !f.method()) {
        field = &f;
      }
    }
    const FunctionDecl *method = nullptr;
    for (const FunctionDecl *fn : cls->methods) {
      if (fn->name == member.name && fn->isStatic() == member.isStatic) {
        method = fn;
      }
    }
    Access access = field ? field->access : method ? method->access() : Access::Public;
    if (!field && !method) {
      this->checker.diags_.error(member.loc,
                                 "'" + cls->name + "' has no " +
                                     (member.isStatic ? "static member '" : "member '") +
                                     member.name + "' in '" + this->fileName() + "'");
      continue;
    }
    if (access == Access::Private) {
      this->checker.diags_.error(member.loc,
                                 what + " is private in '" + this->fileName() +
                                     "': a declaration file declares only what programs "
                                     "can use");
      continue;
    }
    if (member.accessWritten && member.access != access) {
      this->checker.diags_.error(member.loc, what + " is " +
                                                 DeclarationChecker::accessWord(access) + " in '" +
                                                 this->fileName() + "'");
    }
    if (field) {
      this->checkField(member, *field, cls);
    } else {
      this->checkMethod(member, method, cls);
    }
  }
  std::string declarations =
      llvm::sys::path::filename(this->checkedModule->declaration->path).str();
  auto missing = [&](llvm::SMLoc loc, llvm::StringRef name, bool isStatic) {
    this->checker.diags_.error(loc, "'" + name + "' of '" + cls->name + "' is " +
                                        (isStatic ? "a static member" : "a member") +
                                        " that programs can use, but '" + declarations +
                                        "' does not declare it");
  };
  for (const ClassField &f : cls->fields) {
    std::string key = (f.isStatic() ? "static " : "") + f.name.str();
    if (!f.method() && f.access != Access::Private && !seen.contains(key)) {
      missing(f.loc, f.name, f.isStatic());
    }
  }
  for (const FunctionDecl *fn : cls->methods) {
    std::string key = (fn->isStatic() ? "static " : "") + fn->name.str();
    if (fn->access() != Access::Private && !seen.contains(key)) {
      missing(fn->loc(), fn->name, fn->isStatic());
    }
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    if (ctor->access() != Access::Private && !seen.contains("constructor") &&
        !this->mentionsHidden(ctor)) {
      missing(ctor->loc(), "constructor", false);
    }
  }
}

}
