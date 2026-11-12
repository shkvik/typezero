#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

MetadataChecker::MetadataChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->classValueChecker = &all.classValueChecker;
}

ClassDecl *MetadataChecker::builtinConstructorNamed(llvm::StringRef name) {
  auto found = BuiltinConstructorTable::find(name);
  if (!found.has_value()) {
    return nullptr;
  }

  return this->builtinConstructor(*found);
}

ClassDecl *MetadataChecker::builtinConstructor(BuiltinConstructor which) {
  llvm::StringRef name = BuiltinConstructorTable::getName(which);
  auto &reflect = this->state.getReflectStore();
  auto *known = reflect.findBuiltinConstructor(name);
  if (known != nullptr) {
    return known;
  }
  auto *classDecl = this->state.getAst().make<ClassDecl>(llvm::SMLoc(), name);
  classDecl->origin = ClassOrigin::BUILTIN;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  reflect.rememberBuiltinConstructor(name, classDecl);

  return classDecl;
}

MetadataChecker::NamedKey MetadataChecker::metadataKey(const Expr *key) const {
  auto *name = dyn_cast<NameExpr>(key);
  if (name == nullptr) {
    return NamedKey{.key = nullptr, .label = ""};
  }
  auto entry = this->scopes->lookup(name->name);

  return NamedKey{.key = this->entryKey(entry), .label = name->name.str()};
}

const void *MetadataChecker::entryKey(const ScopeEntry &entry) const {
  const VarSymbol *variable = entry.var();
  if (variable != nullptr) {
    bool isKeyConst = variable->isConst && variable->type != nullptr && !variable->type->isString();

    return isKeyConst ? variable : nullptr;
  }
  if (entry.func() != nullptr) {
    return entry.func();
  }

  return entry.cls();
}

void MetadataChecker::walkBodyEarly(FunctionDecl *functionDecl) {
  FuncSymbol *function = functionDecl->sym;
  bool isSettled = function == nullptr || functionDecl->checked || function->isGeneric();
  if (isSettled || this->state.getFunctionStack().isCheckingFunction(functionDecl)) {
    return;
  }
  if (!function->hasReturnType()) {
    this->returnInferrer->inferReturnType(function);
    return;
  }
  if (functionDecl->cls() == nullptr) {
    this->returnInferrer->walkBodyNow(functionDecl);
  }
}

MetadataChecker::ResultMetadataTyped MetadataChecker::metadataType(const void *key,
                                                                   llvm::StringRef label,
                                                                   llvm::SMLoc loc,
                                                                   const Type *expected) {
  const auto *known = this->recordedType(key, label);
  if (known != nullptr) {
    return ok(known->type);
  }
  bool isWritten = expected != nullptr && !expected->nonOptional()->isUnknown() &&
                   !TypeQueries::mentionsParam(expected);
  if (!isWritten) {
    auto message = ErrorTextMetadataType::unknown(label);

    return err(MetadataCheckerMetadataTypeError{
        .code = MetadataCheckerMetadataTypeErrorCode::NO_RECORD_TYPE,
        .message = message,
    });
  }
  const Type *written = expected->nonOptional();
  this->state.getReflectStore().rememberMetadataType(key, written, loc);

  return ok(written);
}

const MetadataType *MetadataChecker::recordedType(const void *key, llvm::StringRef label) {
  auto &reflect = this->state.getReflectStore();
  const MetadataType *known = reflect.findMetadataType(key);
  if (known != nullptr) {
    return known;
  }
  for (FunctionDecl *writer : reflect.getMetadataWriters(label)) {
    this->walkBodyEarly(writer);
    const MetadataType *written = reflect.findMetadataType(key);
    if (written != nullptr) {
      return written;
    }
  }

  return nullptr;
}

Expr *MetadataChecker::designClass(const Type *type, llvm::SMLoc loc) {
  auto *constructor = this->designConstructor(type);

  return this->classValueChecker->classValueAt(constructor, loc);
}

ClassDecl *MetadataChecker::designConstructor(const Type *type) {
  const auto *plain = this->designPlain(type);
  if (!plain->isUnion()) {
    return this->plainConstructor(plain);
  }
  const auto *first = this->designPlain(plain->members().front());
  auto *common = this->plainConstructor(first);
  for (const Type *member : plain->members()) {
    const auto *memberPlain = this->designPlain(member);
    auto *own = this->plainConstructor(memberPlain);
    if (own != common) {
      return this->builtinConstructor(BuiltinConstructor::OBJECT);
    }
  }

  return common;
}

const Type *MetadataChecker::designPlain(const Type *type) const {
  const Type *plain = type->nonOptional();

  return plain->isLiteral() ? plain->literalBase() : plain;
}

ClassDecl *MetadataChecker::plainConstructor(const Type *type) {
  if (type->isBytes()) {
    return this->builtinConstructorNamed(type->name());
  }
  auto builtin = this->builtinDesign(type);
  if (builtin.has_value()) {
    return this->builtinConstructor(*builtin);
  }
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr && classDecl->literal() == nullptr) {
    return classDecl;
  }

  return this->builtinConstructor(BuiltinConstructor::OBJECT);
}

std::optional<BuiltinConstructor> MetadataChecker::builtinDesign(const Type *type) const {
  if (type->isPromise()) {
    return BuiltinConstructor::PROMISE;
  }
  if (type->isNumeric()) {
    return BuiltinConstructor::NUMBER;
  }
  if (type->isString()) {
    return BuiltinConstructor::STRING;
  }
  if (type->isBool()) {
    return BuiltinConstructor::BOOLEAN;
  }
  if (type->isArray() || type->isTuple() || type->isMatch()) {
    return BuiltinConstructor::ARRAY;
  }
  if (type->isFunction() || type->isClassRef()) {
    return BuiltinConstructor::FUNCTION;
  }
  if (type->isDate()) {
    return BuiltinConstructor::DATE;
  }
  if (type->isRegExp()) {
    return BuiltinConstructor::REG_EXP;
  }
  if (type->isMap()) {
    return BuiltinConstructor::MAP;
  }
  if (type->isSet()) {
    return BuiltinConstructor::SET;
  }

  return std::nullopt;
}

void MetadataChecker::writeDesignRecord(ClassDecl *classDecl, DesignKey key, Expr *value,
                                        llvm::StringRef member, llvm::SMLoc loc) {
  auto &ast = this->state.getAst();
  llvm::StringRef reflect = GlobalNameTable::getName(GlobalName::REFLECT);
  auto *callee = ast.make<NameExpr>(loc, reflect);
  callee->type = Type::error();
  auto keyText = ToUtf16::toUtf16(DesignKeyTable::getName(key));
  auto *keyLiteral = ast.make<StringLitExpr>(loc, keyText);
  keyLiteral->type = Type::string();
  Expr *property = this->memberProperty(member, loc);
  Expr *plainValue = this->withoutClassRef(value);
  auto *recorded = this->typeRelations->convert(plainValue, Type::unknown());
  auto *owner = this->classValueChecker->classValueAt(classDecl, loc);
  auto *target = this->typeRelations->convert(owner, Type::anyObject());
  auto *name = this->typeRelations->convert(property, Type::optional(Type::string()));
  auto args = std::vector<Expr *>{keyLiteral, recorded, target, name};
  auto *call = ast.make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = Builtin::MetaDefine};
  call->designRecord = true;
  call->type = Type::voidTy();
  auto *record = ast.make<ExprStmt>(loc, call);
  classDecl->decorations.push_back(record);
}

Expr *MetadataChecker::memberProperty(llvm::StringRef member, llvm::SMLoc loc) {
  auto &ast = this->state.getAst();
  if (member.empty()) {
    Expr *nothing = ast.make<UndefinedExpr>(loc);
    nothing->type = Type::undefinedTy();

    return nothing;
  }
  auto text = std::u16string(member.begin(), member.end());
  Expr *property = ast.make<StringLitExpr>(loc, text);
  property->type = Type::string();

  return property;
}

Expr *MetadataChecker::withoutClassRef(Expr *value) {
  if (!value->type->isClassRef()) {
    return value;
  }

  return this->typeRelations->convert(value, Type::constructorOf(Type::anyObject()));
}

Expr *MetadataChecker::paramClasses(const FunctionDecl *functionDecl, llvm::SMLoc loc) {
  auto *list = this->state.getAst().make<ArrayLitExpr>(loc);
  const auto *any = Type::constructorOf(Type::anyObject());
  for (const Type *param : functionDecl->sym->params) {
    if (param != nullptr) {
      auto *paramClass = this->designClass(param, loc);
      auto *erased = this->typeRelations->convert(paramClass, any);
      auto element = ArrayLitExpr::Element{.expr = erased, .spread = false};
      list->elements.push_back(element);
    }
  }
  list->type = Type::array(any);

  return list;
}

void MetadataChecker::writeDesignRecords(ClassDecl *classDecl, ClassField *field,
                                         FunctionDecl *method, const FunctionDecl *constructor,
                                         llvm::SMLoc loc) {
  if (field != nullptr) {
    this->writeFieldRecord(classDecl, field, loc);
    return;
  }
  if (method != nullptr) {
    this->writeMethodRecords(classDecl, method, loc);
    return;
  }
  if (constructor != nullptr) {
    auto *params = this->paramClasses(constructor, loc);
    this->writeDesignRecord(classDecl, DesignKey::PARAM_TYPES, params, {}, loc);
  }
}

void MetadataChecker::writeFieldRecord(ClassDecl *classDecl, const ClassField *field,
                                       llvm::SMLoc loc) {
  this->classDeclarer->completeClass(classDecl);
  if (field->resolved == nullptr || field->resolved->isError()) {
    return;
  }
  auto *fieldClass = this->designClass(field->resolved, loc);
  this->writeDesignRecord(classDecl, DesignKey::TYPE, fieldClass, field->name, loc);
}

void MetadataChecker::writeMethodRecords(ClassDecl *classDecl, const FunctionDecl *method,
                                         llvm::SMLoc loc) {
  const auto *returnType = method->sym->getReturnType();
  if (returnType != nullptr && !returnType->isVoid() && !returnType->isError()) {
    auto *returnClass = this->designClass(returnType, loc);
    this->writeDesignRecord(classDecl, DesignKey::RETURN_TYPE, returnClass, method->name, loc);
  }
  auto *params = this->paramClasses(method, loc);
  this->writeDesignRecord(classDecl, DesignKey::PARAM_TYPES, params, method->name, loc);
  auto *functionConstructor = this->builtinConstructor(BuiltinConstructor::FUNCTION);
  auto *functionClass = this->classValueChecker->classValueAt(functionConstructor, loc);
  this->writeDesignRecord(classDecl, DesignKey::TYPE, functionClass, method->name, loc);
}

}
