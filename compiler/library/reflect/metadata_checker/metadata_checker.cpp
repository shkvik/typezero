#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

MetadataChecker::MetadataChecker(Sema &sema) : sema_(sema) {}

ClassDecl *MetadataChecker::builtinConstructor(llvm::StringRef name) {
  static const llvm::StringLiteral names[] = {"Number",       "String",
                                              "Boolean",      "Object",
                                              "Array",        "Function",
                                              "Date",         "RegExp",
                                              "Map",          "Set",
                                              "Promise",      "Uint8Array",
                                              "Int8Array",    "Uint8ClampedArray",
                                              "Int16Array",   "Uint16Array",
                                              "Int32Array",   "Uint32Array",
                                              "Float16Array", "Float32Array",
                                              "Float64Array", "Buffer",
                                              "ArrayBuffer",  "DataView",
                                              "TextEncoder",  "TextDecoder"};
  auto found = llvm::find(names, name);
  if (found == std::end(names)) {
    return nullptr;
  }
  ClassDecl *&cls = builtinConstructors_[name];
  if (!cls) {
    cls = sema_.ctx_.make<ClassDecl>(llvm::SMLoc(), *found);
    cls->builtin = true;
    cls->resolved = cls->declared = true;
  }

  return cls;
}

const void *MetadataChecker::metadataKey(const Expr *key, std::string &label) {
  auto *name = dyn_cast<NameExpr>(key);
  if (!name) {
    return nullptr;
  }
  Sema::ScopeEntry entry = sema_.scopes().lookup(name->name);
  label = name->name.str();
  if (entry.var) {
    return entry.var->isConst && entry.var->type && !entry.var->type->isString() ? entry.var
                                                                                 : nullptr;
  }
  if (entry.func) {
    return entry.func;
  }

  return entry.cls;
}

void MetadataChecker::findMetadataWriters(SourceFile &program) {
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      std::vector<FunctionDecl *> fns;
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        fns.push_back(fn);
      } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
        fns = cls->methods;
      }
      for (FunctionDecl *fn : fns) {
        visitExprs(fn, [&](const Expr *e) {
          auto *call = dyn_cast<CallExpr>(e);
          auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
          auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
          if (!object || object->name != "Reflect" || member->name != "defineMetadata" ||
              call->args.empty()) {
            return;
          }
          if (auto *key = dyn_cast<NameExpr>(call->args[0])) {
            std::vector<FunctionDecl *> &writers = metaWriters_[key->name];
            if (!llvm::is_contained(writers, fn)) {
              writers.push_back(fn);
            }
          }
        });
      }
    }
  }
}

void MetadataChecker::checkBodyEarly(FunctionDecl *fn) {
  if (!fn->sym || fn->checked || fn->sym->isGeneric() || llvm::is_contained(sema_.functions_, fn)) {
    return;
  }
  if (!fn->sym->ret) { // checking it finds what it returns
    sema_.returnInferrer().inferReturnType(fn->sym);
    return;
  }
  if (fn->cls) { // checked with its class
    return;
  }
  Sema::BodyState saved = sema_.returnInferrer().enterBody(fn);
  if (!sema_.topLevelDone_) {
    sema_.early_ = fn->sym; // top-level code reads it: globals declared later are not there yet
  }
  sema_.flow_ = Sema::Flow();
  sema_.functionChecker().checkFunctionBody(fn);
  sema_.returnInferrer().leaveBody(saved);
}

const Type *MetadataChecker::metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                               const Type *expected) {
  if (auto found = sema_.metaTypes_.find(key); found != sema_.metaTypes_.end()) {
    return found->second.type;
  }
  if (auto writers = metaWriters_.find(label); writers != metaWriters_.end()) {
    for (FunctionDecl *fn : std::vector<FunctionDecl *>(writers->second)) {
      checkBodyEarly(fn);
      if (auto found = sema_.metaTypes_.find(key); found != sema_.metaTypes_.end()) {
        return found->second.type;
      }
    }
  }
  // `const names: string[] = Reflect.getMetadata(Column, cls) ?? []` in the
  // function that writes them next.
  if (expected && !expected->nonOptional()->isUnknown() && !mentionsParam(expected)) {
    sema_.metaTypes_[key] = {expected->nonOptional(), loc};
    return expected->nonOptional();
  }
  sema_.diags_.error(loc,
               "the records of '" + label +
                   "' have the type of the first one written, "
                   "'Reflect.defineMetadata(" +
                   label +
                   ", value, target)', and none is written "
                   "before this; write one first, give the type where this one goes (e.g. "
                   "'const x: string[] = ...'), or use a string key, whose records are 'unknown'");

  return Type::error();
}

Expr *MetadataChecker::designClass(const Type *type, llvm::SMLoc loc) {
  return sema_.classValueChecker().classValueAt(designConstructor(type), loc);
}

ClassDecl *MetadataChecker::designConstructor(const Type *type) {
  type = type->nonOptional();
  // A literal type is a value of its base; a union is of the constructor all
  // its members have, or else Object, as TypeScript writes them.
  if (type->isLiteral()) {
    type = type->literalBase();
  }
  if (type->isUnion()) {
    ClassDecl *common = designConstructor(type->members().front());
    for (const Type *m : type->members()) {
      if (designConstructor(m) != common) {
        return builtinConstructor("Object");
      }
    }
    return common;
  }
  if (type->isBytes()) {
    return builtinConstructor(type->name());
  }
  llvm::StringRef name = "Object";
  if (type->isPromise()) {
    name = "Promise";
  } else if (type->isNumeric()) {
    name = "Number";
  } else if (type->isString()) {
    name = "String";
  } else if (type->isBool()) {
    name = "Boolean";
  } else if (type->isArray() || type->isTuple() || type->isMatch()) {
    name = "Array";
  } else if (type->isFunction() || type->isClassRef()) {
    name = "Function";
  } else if (type->isDate()) {
    name = "Date";
  } else if (type->isRegExp()) {
    name = "RegExp";
  } else if (type->isMap()) {
    name = "Map";
  } else if (type->isSet()) {
    name = "Set";
  } else if (ClassDecl *cls = ClassDeclarer::classOf(type); cls && !cls->literal) {
    return cls;
  }

  return builtinConstructor(name);
}

void MetadataChecker::writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value,
                             llvm::StringRef member, llvm::SMLoc loc) {
  auto *callee = sema_.ctx_.make<NameExpr>(loc, "Reflect");
  callee->type = Type::error(); // the call is a built-in, which needs none
  auto *keyLit = sema_.ctx_.make<StringLitExpr>(loc, key);
  keyLit->type = Type::string();
  Expr *property;
  if (member.empty()) {
    property = sema_.ctx_.make<UndefinedExpr>(loc);
    property->type = Type::undefinedTy();
  } else {
    property = sema_.ctx_.make<StringLitExpr>(loc, std::u16string(member.begin(), member.end()));
    property->type = Type::string();
  }
  if (value->type->isClassRef()) {
    value = sema_.typeRelations().convert(value, Type::constructorOf(Type::anyObject()));
  }
  std::vector<Expr *> args{keyLit, sema_.typeRelations().convert(value, Type::unknown()),
                           sema_.typeRelations().convert(sema_.classValueChecker().classValueAt(cls, loc), Type::anyObject()),
                           sema_.typeRelations().convert(property, Type::optional(Type::string()))};
  auto *call = sema_.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->builtin = Builtin::MetaDefine;
  call->designRecord = true;
  call->type = Type::voidTy();
  cls->decorations.push_back(sema_.ctx_.make<ExprStmt>(loc, call));
}

// As TypeScript's emitDecoratorMetadata writes them, here always: in the
// order TypeScript applies them, design:returntype first.
void MetadataChecker::writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                              const FunctionDecl *ctor, llvm::SMLoc loc) {
  auto paramTypes = [&](const FunctionDecl *fn) {
    auto *list = sema_.ctx_.make<ArrayLitExpr>(loc);
    sema_.returnInferrer().resolveParams(fn->sym);
    const Type *any = Type::constructorOf(Type::anyObject());
    for (const Type *param : fn->sym->params) {
      if (param) {
        list->elements.push_back({sema_.typeRelations().convert(designClass(param, loc), any), false});
      }
    }
    list->type = Type::array(any);

    return list;
  };
  if (field) {
    sema_.classDeclarer().completeClass(cls); // the types that come from the initializers
    if (field->resolved && !field->resolved->isError()) {
      writeDesignRecord(cls, u"design:type", designClass(field->resolved, loc), field->name, loc);
    }
    return;
  }
  if (method) {
    const Type *ret = sema_.returnInferrer().returnType(method->sym);
    if (ret && !ret->isVoid() && !ret->isError()) {
      writeDesignRecord(cls, u"design:returntype", designClass(ret, loc), method->name, loc);
    }
    writeDesignRecord(cls, u"design:paramtypes", paramTypes(method), method->name, loc);
    writeDesignRecord(cls, u"design:type", sema_.classValueChecker().classValueAt(builtinConstructor("Function"), loc),
                      method->name, loc);
    return;
  }
  if (ctor) {
    writeDesignRecord(cls, u"design:paramtypes", paramTypes(ctor), {}, loc);
  }
}

} // namespace tz
