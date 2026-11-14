#include "compiler/semantic/types_v2/typeof/types_typeof_registry.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

TypesTypeofRegistry::TypesTypeofRegistry(SemanticState &owner)
    : state(owner) {}

void TypesTypeofRegistry::evalFunctionPart(const EvalFunctionPart &task) {
  const Type *functionType = task.operand->nonOptional();
  if (!functionType->isFunction()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  if (task.kind == TypeOp::ReturnType) {
    return this->state.walk.tasks.pushAnswer(functionType->ret());
  }
  llvm::ArrayRef<const Type *> all = functionType->params();
  llvm::ArrayRef<const Type *> fixed = functionType->hasRest() ? all.drop_back() : all;
  const Type *rest = functionType->hasRest() ? all.back() : nullptr;
  size_t required = std::min(functionType->minArgs(), fixed.size());
  const auto *list = Type::argumentList(fixed, required, rest);
  this->state.walk.tasks.pushAnswer(list);
}

void TypesTypeofRegistry::startTypeOf(const StartTypeOf &task) {
  llvm::StringRef name = task.ref->names().front();
  auto load = this->state.findEarlyLoad(name);
  if (!load.has_value()) {
    return this->findTypeOfCandidates(FindTypeOfCandidates{.ref = task.ref});
  }
  this->state.walk.tasks.pushTask(FindTypeOfCandidates{.ref = task.ref});
  this->state.walk.tasks.pushTask(*load);
}

void TypesTypeofRegistry::findTypeOfCandidates(const FindTypeOfCandidates &task) {
  llvm::StringRef name = task.ref->names().front();
  auto draft = TypeOfDraft{
      .ref = task.ref,
      .candidates = this->typeOfCandidates(name),
      .next = 0,
      .type = nullptr,
      .part = 1,
  };

  return this->nextTypeOfCandidate(draft);
}

std::vector<TypeOfCandidate> TypesTypeofRegistry::typeOfCandidates(llvm::StringRef name) const {
  auto candidates = std::vector<TypeOfCandidate>();
  this->addScopedCandidates(name, candidates);
  auto *module = this->state.declaration.modules.getCurrentModule();
  if (module == nullptr) {
    return candidates;
  }
  for (Stmt *item : module->file.items) {
    this->addItemCandidates(item, name, candidates);
  }
  for (const ImportDecl &importDecl : module->file.imports) {
    this->addImportCandidates(importDecl, name, candidates);
  }

  return candidates;
}

void TypesTypeofRegistry::addScopedCandidates(llvm::StringRef name,
                                              std::vector<TypeOfCandidate> &candidates) const {
  auto entry = this->state.walk.scopes.findName(name);
  VarSymbol *variable = entry.var();
  ClassDecl *classDecl = entry.cls();
  FuncSymbol *function = entry.func();
  if (variable != nullptr) {
    return candidates.push_back(this->knownCandidate(variable->type));
  }
  if (classDecl != nullptr && !classDecl->isGeneric()) {
    return candidates.push_back(this->classCandidate(classDecl));
  }
  if (function != nullptr) {
    candidates.push_back(this->functionCandidate(function));
  }
}

void TypesTypeofRegistry::addItemCandidates(Stmt *item, llvm::StringRef name,
                                            std::vector<TypeOfCandidate> &candidates) const {
  auto *varDecl = dyn_cast<VarDeclStmt>(item);
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (varDecl != nullptr) {
    return this->addVarCandidates(varDecl, name, candidates);
  }
  if (functionDecl != nullptr && this->isDeclaringFunctionType(functionDecl, name)) {
    candidates.push_back(this->declaredFunctionCandidate(functionDecl));
  }
}

void TypesTypeofRegistry::addVarCandidates(const VarDeclStmt *varDecl, llvm::StringRef name,
                                           std::vector<TypeOfCandidate> &candidates) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.name != name) {
      continue;
    }
    if (declarator.typeAnn.has_value()) {
      candidates.push_back(this->resolvedCandidate(&*declarator.typeAnn));
    } else if (declarator.init != nullptr) {
      candidates.push_back(this->shapeCandidate(declarator.init));
    }
  }
  for (const VarDeclarator &declarator : llvm::reverse(varDecl->decls)) {
    auto *create = llvm::dyn_cast_or_null<NewExpr>(declarator.init);
    if (declarator.name == name && create != nullptr) {
      candidates.push_back(this->createdCandidate(create));
    }
  }
}

bool TypesTypeofRegistry::isDeclaringFunctionType(const FunctionDecl *functionDecl,
                                                  llvm::StringRef name) const {
  return functionDecl->name == name && functionDecl->typeParams.empty() &&
         functionDecl->returnType.has_value();
}

size_t TypesTypeofRegistry::typedParamCount(const FunctionDecl *functionDecl) const {
  size_t count = 0;
  while (count < functionDecl->params.size() && functionDecl->params[count].type.has_value()) {
    count += 1;
  }

  return count;
}

void TypesTypeofRegistry::addImportCandidates(const ImportDecl &importDecl, llvm::StringRef name,
                                              std::vector<TypeOfCandidate> &candidates) const {
  if (importDecl.from == nullptr) {
    return;
  }
  for (const ModuleName &imported : importDecl.names) {
    if (imported.as == name) {
      this->addImportedCandidate(importDecl.from, imported.name, candidates);
    }
  }
}

void TypesTypeofRegistry::addImportedCandidate(Module *from, llvm::StringRef name,
                                               std::vector<TypeOfCandidate> &candidates) const {
  for (Stmt *item : from->file.items) {
    auto *functionDecl = dyn_cast<FunctionDecl>(item);
    auto *classDecl = dyn_cast<ClassDecl>(item);
    auto *varDecl = dyn_cast<VarDeclStmt>(item);
    if (functionDecl != nullptr && functionDecl->name == name) {
      if (functionDecl->sym != nullptr) {
        candidates.push_back(this->functionCandidate(functionDecl->sym));
      }
      return;
    }
    if (classDecl != nullptr && classDecl->name == name) {
      if (!classDecl->isGeneric()) {
        candidates.push_back(this->classCandidate(classDecl));
      }
      return;
    }
    const auto *declarator = varDecl != nullptr ? this->declaratorNamed(varDecl, name) : nullptr;
    if (declarator != nullptr) {
      const Type *known = declarator->sym != nullptr ? declarator->sym->type : nullptr;
      return candidates.push_back(this->knownCandidate(known));
    }
  }
}

const VarDeclarator *TypesTypeofRegistry::declaratorNamed(const VarDeclStmt *varDecl,
                                                          llvm::StringRef name) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.name == name) {
      return &declarator;
    }
  }

  return nullptr;
}

TypeOfCandidate TypesTypeofRegistry::knownCandidate(const Type *known) const {
  return TypeOfCandidate{
      .source = TypeOfSource::KNOWN,
      .known = known,
      .ref = nullptr,
      .init = nullptr,
      .create = nullptr,
      .function = nullptr,
      .classDecl = nullptr,
      .functionDecl = nullptr,
  };
}

TypeOfCandidate TypesTypeofRegistry::resolvedCandidate(const TypeRef *ref) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::RESOLVED;
  candidate.ref = ref;

  return candidate;
}

TypeOfCandidate TypesTypeofRegistry::shapeCandidate(const Expr *init) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::LITERAL_SHAPE;
  candidate.init = init;

  return candidate;
}

TypeOfCandidate TypesTypeofRegistry::createdCandidate(const NewExpr *create) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::CREATED;
  candidate.create = create;

  return candidate;
}

TypeOfCandidate TypesTypeofRegistry::functionCandidate(FuncSymbol *function) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::FUNCTION_VALUE;
  candidate.function = function;

  return candidate;
}

TypeOfCandidate TypesTypeofRegistry::classCandidate(ClassDecl *classDecl) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::CLASS_VALUE;
  candidate.classDecl = classDecl;

  return candidate;
}

TypeOfCandidate
TypesTypeofRegistry::declaredFunctionCandidate(const FunctionDecl *functionDecl) const {
  auto candidate = this->knownCandidate(nullptr);
  candidate.source = TypeOfSource::DECLARED_FUNCTION;
  candidate.functionDecl = functionDecl;

  return candidate;
}

void TypesTypeofRegistry::nextTypeOfCandidate(TypeOfDraft draft) {
  if (draft.next == draft.candidates.size()) {
    return this->reportUnknownTypeOf(draft);
  }
  const TypeOfCandidate &candidate = draft.candidates[draft.next];
  if (candidate.source == TypeOfSource::KNOWN && candidate.known != nullptr) {
    return this->nextTypeOfMember(draft, candidate.known);
  }
  if (candidate.source == TypeOfSource::KNOWN) {
    draft.next += 1;

    return this->nextTypeOfCandidate(draft);
  }
  this->state.walk.tasks.pushTask(AfterTypeOfCandidate{.draft = draft});

  return this->pushCandidateType(candidate);
}

void TypesTypeofRegistry::pushCandidateType(const TypeOfCandidate &candidate) {
  switch (candidate.source) {
    case TypeOfSource::KNOWN:
      return this->state.walk.tasks.pushAnswer(candidate.known);
    case TypeOfSource::RESOLVED:
      return this->state.walk.tasks.pushTask(
          ResolveTypeAnswer{.ref = candidate.ref, .use = TypeUse::STANDALONE});
    case TypeOfSource::LITERAL_SHAPE:
      return this->state.walk.tasks.pushTask(LiteralShape{.init = candidate.init});
    case TypeOfSource::CREATED:
      return this->state.walk.tasks.pushTask(CreatedType{.create = candidate.create});
    case TypeOfSource::CLASS_VALUE:
      return this->state.walk.tasks.pushTask(ClassValueType{.classDecl = candidate.classDecl});
    case TypeOfSource::FUNCTION_VALUE:
      return this->pushFunctionValue(candidate.function);
    case TypeOfSource::DECLARED_FUNCTION:
      return this->pushDeclaredFunction(candidate.functionDecl);
  }
}

void TypesTypeofRegistry::afterTypeOfCandidate(const AfterTypeOfCandidate &task) {
  TypeOfDraft draft = task.draft;
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (type != nullptr) {
    return this->nextTypeOfMember(draft, type);
  }
  draft.next += 1;

  return this->nextTypeOfCandidate(draft);
}

void TypesTypeofRegistry::reportUnknownTypeOf(const TypeOfDraft &draft) {
  const TypeRef &ref = *draft.ref;
  auto message = ErrorTextTypeOf::typeOf(ref.names().front());
  this->state.diagnostics.error(ref.loc, message);
  this->state.walk.tasks.pushAnswer(Type::error());
}

void TypesTypeofRegistry::nextTypeOfMember(TypeOfDraft draft, const Type *type) {
  const TypeRef &ref = *draft.ref;
  draft.type = type;
  while (draft.part < ref.names().size()) {
    llvm::StringRef part = ref.names()[draft.part];
    const Field *field = type->isObject() ? type->field(part) : nullptr;
    if (field == nullptr) {
      return this->pushSingleMethod(draft);
    }
    type = field->type;
    draft.type = type;
    draft.part += 1;
  }
  this->state.walk.tasks.pushAnswer(type);
}

void TypesTypeofRegistry::pushSingleMethod(const TypeOfDraft &draft) {
  const TypeRef &ref = *draft.ref;
  llvm::StringRef part = ref.names()[draft.part];
  FuncSymbol *function = this->singleMethod(draft.type, part);
  if (function == nullptr) {
    auto message = ErrorTextTypeOf::noField(draft.type->name(), part);
    this->state.diagnostics.error(ref.loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(AfterTypeOfMember{.draft = draft, .function = function});
  this->state.walk.tasks.pushTask(ReturnTypeOf{.function = function});
}

FuncSymbol *TypesTypeofRegistry::singleMethod(const Type *type, llvm::StringRef name) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(name);
  bool isSingle =
      methods.size() == 1 && methods[0]->sym != nullptr && methods[0]->typeParams.empty();

  return isSingle ? methods[0]->sym : nullptr;
}

void TypesTypeofRegistry::afterTypeOfMember(const AfterTypeOfMember &task) {
  TypeOfDraft draft = task.draft;
  const Type *returned = this->state.walk.tasks.takeAnswer();
  const auto *type = this->functionTypeWith(task.function, returned);
  draft.part += 1;

  return this->nextTypeOfMember(draft, type);
}

const Type *TypesTypeofRegistry::functionTypeWith(const FuncSymbol *function,
                                                  const Type *returnType) const {
  const Type *returned = returnType != nullptr ? returnType : Type::error();

  return Type::function(function->params, function->minArgs, function->hasRest, returned,
                        function->hasThis);
}

void TypesTypeofRegistry::pushFunctionValue(FuncSymbol *function) {
  bool hasAllDefaults = function->typeDefaults.size() == function->typeParams.size() &&
                        !llvm::is_contained(function->typeDefaults, nullptr);
  if (function->isGeneric() && !hasAllDefaults) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  this->state.walk.tasks.pushTask(AfterFunctionValue{.function = function});
  this->state.walk.tasks.pushTask(ReturnTypeOf{.function = function});
}

void TypesTypeofRegistry::afterFunctionValue(const AfterFunctionValue &task) {
  FuncSymbol *function = task.function;
  const Type *returned = this->state.walk.tasks.takeAnswer();
  const auto *type = this->functionTypeWith(function, returned);
  if (!function->isGeneric()) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  this->state.walk.tasks.pushTask(SubstituteType{
      .type = type,
      .params = function->typeParams,
      .args = function->typeDefaults,
  });
}

void TypesTypeofRegistry::pushDeclaredFunction(const FunctionDecl *functionDecl) {
  size_t typed = this->typedParamCount(functionDecl);
  this->state.walk.tasks.pushTask(
      FinishDeclaredFunctionValue{.functionDecl = functionDecl, .typedCount = typed});
  if (typed == functionDecl->params.size()) {
    this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &*functionDecl->returnType, .use = TypeUse::STANDALONE_OR_VOID});
  }
  for (size_t index = typed; index > 0; --index) {
    const Param &param = functionDecl->params[index - 1];
    this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &*param.type, .use = TypeUse::STANDALONE});
  }
}

void TypesTypeofRegistry::finishDeclaredFunctionValue(const FinishDeclaredFunctionValue &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  bool isComplete = task.typedCount == functionDecl->params.size();
  const Type *returned = isComplete ? this->state.walk.tasks.takeAnswer() : nullptr;
  auto resolved = std::vector<const Type *>(task.typedCount, nullptr);
  for (size_t index = resolved.size(); index > 0; --index) {
    resolved[index - 1] = this->state.walk.tasks.takeAnswer();
  }
  if (!isComplete) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  auto params = std::vector<const Type *>();
  size_t minArgs = 0;
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    const Param &param = functionDecl->params[index];
    bool isOptional = param.optional() || param.defaultValue() != nullptr;
    params.push_back(isOptional ? Type::optional(resolved[index]) : resolved[index]);
    if (!isOptional && !param.rest()) {
      minArgs = params.size();
    }
  }
  bool hasRest = !functionDecl->params.empty() && functionDecl->params.back().rest();
  const auto *function = Type::function(params, minArgs, hasRest, returned);
  this->state.walk.tasks.pushAnswer(function);
}

void TypesTypeofRegistry::signatureForUtility(const SignatureForUtility &task) {
  const TypeRef &ref = *task.ref;
  size_t count = ref.names().size();
  bool isNamed = ref.kind() == TypeRefKind::TYPE_OF_REF && (count == 1 || count == 2);
  if (!isNamed) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  auto load = this->state.findEarlyLoad(ref.names().front());
  if (!load.has_value()) {
    return this->continueSignature(ContinueSignature{.ref = &ref});
  }
  this->state.walk.tasks.pushTask(ContinueSignature{.ref = &ref});
  this->state.walk.tasks.pushTask(*load);
}

void TypesTypeofRegistry::continueSignature(const ContinueSignature &task) {
  const TypeRef &ref = *task.ref;
  auto entry = this->state.walk.scopes.findName(ref.names().front());
  FuncSymbol *function = ref.names().size() == 1 ? entry.func() : nullptr;
  if (ref.names().size() == 1 && (function == nullptr || !function->isGeneric())) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  if (function != nullptr) {
    return this->pushErasedSignature(function);
  }
  VarSymbol *variable = entry.var();
  if (variable == nullptr || variable->type == nullptr) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  const Type *object = variable->type->nonOptional();
  llvm::StringRef methodName = ref.names()[1];
  const Overload *overload = object->isObject() ? object->overload(methodName) : nullptr;
  if (overload != nullptr) {
    return this->state.walk.tasks.pushAnswer(overload->signatures.back());
  }
  FuncSymbol *method = this->classMethod(object, methodName);
  if (method == nullptr) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }

  return this->pushErasedSignature(method);
}

FuncSymbol *TypesTypeofRegistry::classMethod(const Type *object, llvm::StringRef methodName) const {
  const ClassDecl *classDecl = object->classDecl();
  if (classDecl == nullptr || object->field(methodName) != nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(methodName);
  if (methods.empty()) {
    return nullptr;
  }

  return methods.back()->sym;
}

void TypesTypeofRegistry::pushErasedSignature(FuncSymbol *function) {
  this->state.walk.tasks.pushTask(AfterErasedReturn{.function = function});
  this->state.walk.tasks.pushTask(ReturnTypeOf{.function = function});
}

void TypesTypeofRegistry::afterErasedReturn(const AfterErasedReturn &task) {
  FuncSymbol *function = task.function;
  const Type *returned = this->state.walk.tasks.takeAnswer();
  const auto *type = this->functionTypeWith(function, returned);
  auto args = std::vector<const Type *>();
  for (const Type *param : function->typeParams) {
    args.push_back(this->erasedParam(param));
  }
  this->state.walk.tasks.pushTask(
      SubstituteType{.type = type, .params = function->typeParams, .args = args});
}

const Type *TypesTypeofRegistry::erasedParam(const Type *param) const {
  if (param->constraint() != nullptr) {
    return param->constraint();
  }
  if (!param->alternatives().empty()) {
    return Type::unionOf(param->alternatives());
  }
  if (param->isListParam()) {
    return Type::array(Type::unknown());
  }

  return Type::unknown();
}

}
