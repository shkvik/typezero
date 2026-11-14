#include "compiler/semantic/code/classes/class_members/instance_of_checks/instance_of_checks.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"

using llvm::dyn_cast;

namespace tz {

InstanceOfChecks::InstanceOfChecks(SemanticState &owner)
    : state(owner) {}

void InstanceOfChecks::checkInstanceOfOperator(const CheckInstanceOfOperator &task) {
  BinaryExpr *binary = task.binary;
  this->state.walk.tasks.pushTask(AfterInstanceOfLeft{.binary = binary});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = nullptr});
}

void InstanceOfChecks::afterInstanceOfLeft(const AfterInstanceOfLeft &task) {
  BinaryExpr *binary = task.binary;
  binary->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(binary->rhs);
  auto load = name != nullptr ? this->state.findEarlyLoad(name->name) : std::nullopt;
  if (!load.has_value()) {
    return this->instanceOfRight(InstanceOfRight{.binary = binary});
  }
  this->state.walk.tasks.pushTask(InstanceOfRight{.binary = binary});

  return this->state.walk.tasks.pushTask(*load);
}

void InstanceOfChecks::instanceOfRight(const InstanceOfRight &task) {
  BinaryExpr *binary = task.binary;
  auto *name = dyn_cast<NameExpr>(binary->rhs);
  ClassDecl *classDecl =
      name != nullptr ? this->state.walk.scopes.findName(name->name).cls() : nullptr;
  std::optional<GlobalName> global =
      classDecl == nullptr ? this->collectionGlobal(binary->rhs) : std::nullopt;
  if (global.has_value()) {
    auto tested = this->checkCollectionInstanceOf(binary, *global);
    return this->pushInstanceOf(binary, tested);
  }
  const Type *bytes = classDecl == nullptr ? this->namedBytesType(name) : nullptr;
  if (bytes != nullptr) {
    auto tested = this->checkBytesTest(binary, bytes);
    return this->pushInstanceOf(binary, tested);
  }
  if (classDecl == nullptr) {
    this->state.walk.tasks.pushTask(AfterInstanceOfRight{.binary = binary});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &binary->rhs, .expected = nullptr});
  }
  auto tested = this->checkKnownClass(binary, classDecl);

  return this->pushInstanceOf(binary, tested);
}

void InstanceOfChecks::afterInstanceOfRight(const AfterInstanceOfRight &task) {
  BinaryExpr *binary = task.binary;
  auto *known = this->knownClassOf(binary->rhs->type);
  auto tested =
      known != nullptr ? this->checkKnownClass(binary, known) : this->checkUnknownRight(binary);

  return this->pushInstanceOf(binary, tested);
}

void InstanceOfChecks::pushInstanceOf(BinaryExpr *binary, const ResultInstanceTested &tested) {
  if (tested.isErr()) {
    auto at = this->instanceOfErrorPlace(binary, tested.error().code);
    this->state.diagnostics.error(at, tested.error().message);
    return this->state.walk.tasks.pushChecked(binary);
  }

  return this->state.walk.tasks.pushChecked(tested.value());
}

llvm::SMLoc
InstanceOfChecks::instanceOfErrorPlace(const BinaryExpr *binary,
                                       CodeClassesRegistryCheckInstanceOfErrorCode code) const {
  switch (code) {
    case CodeClassesRegistryCheckInstanceOfErrorCode::NOT_AN_OBJECT:
      return binary->lhs->loc();
    case CodeClassesRegistryCheckInstanceOfErrorCode::NOT_A_CLASS:
      return binary->rhs->loc();
    case CodeClassesRegistryCheckInstanceOfErrorCode::UNTYPED_COLLECTION:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NONE_MATCHES:
    case CodeClassesRegistryCheckInstanceOfErrorCode::ALL_MATCH:
    case CodeClassesRegistryCheckInstanceOfErrorCode::ALWAYS_TRUE:
    case CodeClassesRegistryCheckInstanceOfErrorCode::TEST_UNDEFINED:
    case CodeClassesRegistryCheckInstanceOfErrorCode::ALWAYS_FALSE:
    case CodeClassesRegistryCheckInstanceOfErrorCode::BYTES_INSTANCE:
    case CodeClassesRegistryCheckInstanceOfErrorCode::GENERIC_UNKNOWN:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_GENERIC:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_IN_UNION:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_CLASS:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NOT_IMPLEMENTED:
    case CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_CREATED:
      return binary->loc();
  }

  return binary->loc();
}

InstanceOfChecks::ResultInstanceTested InstanceOfChecks::checkBytesTest(BinaryExpr *binary,
                                                                        const Type *bytes) {
  auto tested = this->checkBytesInstanceOf(binary, bytes);
  if (tested.isErr()) {
    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::BYTES_INSTANCE,
        .message = tested.error().message,
    });
  }

  return ok(tested.value());
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkKnownClass(BinaryExpr *binary, const ClassDecl *classDecl) const {
  if (classDecl->isGeneric()) {
    return this->checkGenericInstanceOf(binary, classDecl);
  }

  return this->checkClassInstanceOf(binary, classDecl);
}

std::optional<GlobalName> InstanceOfChecks::collectionGlobal(const Expr *rhs) const {
  static const GlobalName collections[] = {GlobalName::DATE, GlobalName::MAP, GlobalName::SET};
  for (GlobalName global : collections) {
    if (this->state.isGlobalName(rhs, global)) {
      return global;
    }
  }

  return std::nullopt;
}

bool InstanceOfChecks::isCollection(GlobalName global, const Type *type) const {
  if (global == GlobalName::DATE) {
    return type->isDate();
  }
  if (global == GlobalName::MAP) {
    return type->isMap();
  }

  return type->isSet();
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkCollectionInstanceOf(BinaryExpr *binary, GlobalName global) const {
  auto builtin = GlobalNameTable::getName(global);
  const Type *type = binary->lhs->type;
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(builtin);
  bool isOpaque = base->isUnknown() || base->isAnyObject();
  if (isOpaque && global != GlobalName::DATE) {
    llvm::StringRef example = global == GlobalName::MAP ? "Map<string, int>" : "Set<string>";
    auto message = ErrorTextInstanceOf::untypedMap(builtin, example);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::UNTYPED_COLLECTION,
        .message = message,
    });
  }
  if (isOpaque) {
    binary->rhs->type = Type::date();
    return ok(binary);
  }
  if (base->isUnion()) {
    return this->checkCollectionUnion(binary, global);
  }
  if (this->isCollection(global, type)) {
    auto message = ErrorTextInstanceOf::alwaysTrue(type->name(), builtin);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::ALWAYS_TRUE,
        .message = message,
    });
  }
  if (type->isOptional() && this->isCollection(global, type->inner())) {
    auto message = ErrorTextInstanceOf::orUndef(type->inner()->name());

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::TEST_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextInstanceOf::neverIs(type->name(), what, builtin);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::ALWAYS_FALSE,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkCollectionUnion(BinaryExpr *binary, GlobalName global) const {
  auto builtin = GlobalNameTable::getName(global);
  const Type *type = binary->lhs->type;
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(builtin);
  auto matching = std::vector<const Type *>();
  for (const Type *member : base->members()) {
    if (this->isCollection(global, member)) {
      matching.push_back(member);
    }
  }
  if (matching.empty()) {
    auto message = ErrorTextInstanceOf::noneIs(base->name(), what);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NONE_MATCHES,
        .message = message,
    });
  }
  if (matching.size() == base->members().size() && !type->isOptional()) {
    auto message = ErrorTextInstanceOf::allAre(base->name(), what, builtin);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::ALL_MATCH,
        .message = message,
    });
  }
  binary->rhs->type = Type::unionOf(matching);

  return ok(binary);
}

const Type *InstanceOfChecks::namedBytesType(const NameExpr *name) const {
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);
  bool isLocal = entry.var() != nullptr || entry.func() != nullptr;

  return isLocal ? nullptr : this->state.library.bytes.bytesTypeNamed(name->name);
}

ClassDecl *InstanceOfChecks::knownClassOf(const Type *rhs) const {
  auto *known = const_cast<ClassDecl *>(rhs->classRef());

  return known != nullptr && known->type != nullptr ? known : nullptr;
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkUnknownRight(BinaryExpr *binary) const {
  const Type *rhs = binary->rhs->type;
  if (rhs->isConstructor()) {
    return this->checkConstructorRight(binary);
  }
  if (!rhs->isError()) {
    auto message = ErrorTextInstanceOf::notClass();

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkConstructorRight(BinaryExpr *binary) const {
  const Type *base = binary->lhs->type->nonOptional();
  bool isObject = base->isError() || base->isUnknown() || UnknownChecker::isObjectReference(base);
  if (!isObject) {
    auto message = ErrorTextInstanceOf::notObject(binary->lhs->type->name());

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkGenericInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const {
  const Type *type = binary->lhs->type;
  binary->rhs->type = Type::error();
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  auto matching = std::vector<const Type *>();
  for (const Type *member : members) {
    if (GenericClassChecker::genericInstanceOf(member, classDecl) != nullptr) {
      matching.push_back(member);
    }
  }
  if (matching.empty()) {
    return this->checkGenericRelation(base, members, classDecl);
  }
  binary->rhs->type = this->genericTestType(base, matching, classDecl);

  return ok(binary);
}

const Type *InstanceOfChecks::genericTestType(const Type *base,
                                              llvm::ArrayRef<const Type *> matching,
                                              const ClassDecl *classDecl) const {
  if (!base->isUnion()) {
    return GenericClassChecker::genericInstanceOf(base, classDecl)->type;
  }
  if (matching.size() == 1) {
    return matching[0];
  }

  return Type::unionOf(matching);
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkGenericRelation(const Type *base, llvm::ArrayRef<const Type *> members,
                                       const ClassDecl *classDecl) const {
  if (this->isRelatedToGeneric(base, members, classDecl)) {
    std::string generic = classDecl->name.str();
    bool isOpaque = base->isUnknown() || base->isAnyObject();
    std::string other = isOpaque ? generic + "<string>" : base->name().str();
    std::string hint = classDecl->library() ? ErrorTextInstanceOf::extendHint(generic)
                                            : ErrorTextInstanceOf::unionHint(generic, other);
    auto message = ErrorTextInstanceOf::cantTell(generic, base->name(), hint);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::GENERIC_UNKNOWN,
        .message = message,
    });
  }
  auto message = ErrorTextInstanceOf::neverThat(base->name(), classDecl->name);

  return err(CodeClassesRegistryCheckInstanceOfError{
      .code = CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_GENERIC,
      .message = message,
  });
}

bool InstanceOfChecks::isRelatedToGeneric(const Type *base, llvm::ArrayRef<const Type *> members,
                                          const ClassDecl *classDecl) const {
  bool isBaseRelated = base->isUnknown() || base->isAnyObject() ||
                       (base->isObject() && classDecl->type->isSubtypeOf(base));
  if (isBaseRelated) {
    return true;
  }
  for (const Type *member : members) {
    if (member->isObject() && classDecl->type->isSubtypeOf(member)) {
      return true;
    }
  }

  return false;
}

bool InstanceOfChecks::canHoldClass(const Type *base, const ClassDecl *classDecl) const {
  for (const Type *member : base->members()) {
    bool isRelated = classDecl->type->isSubtypeOf(member) || member->isSubtypeOf(classDecl->type);
    if (member->isObject() && isRelated) {
      return true;
    }
  }

  return false;
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkClassInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const {
  binary->rhs->type = classDecl->type;
  const Type *type = binary->lhs->type;
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(binary);
  }
  if (base->isUnion() && !this->canHoldClass(base, classDecl)) {
    auto message = ErrorTextInstanceOf::neverUnion(base->name(), classDecl->name);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_IN_UNION,
        .message = message,
    });
  }
  if (base->isUnion()) {
    return ok(binary);
  }
  if (!base->isObject()) {
    auto message = ErrorTextInstanceOf::notObject(type->name());

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return this->checkRelatedClass(binary, base, classDecl);
}

InstanceOfChecks::ResultInstanceTested
InstanceOfChecks::checkRelatedClass(BinaryExpr *binary, const Type *base,
                                    const ClassDecl *classDecl) const {
  bool isRelated = classDecl->type->isSubtypeOf(base) || base->isSubtypeOf(classDecl->type);
  if (!isRelated && base->isClass()) {
    auto message = ErrorTextInstanceOf::neverClass(base->name(), classDecl->name);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_CLASS,
        .message = message,
    });
  }
  if (!isRelated && (!base->bases().empty() || base->isBase())) {
    auto message = ErrorTextInstanceOf::notImpl(base->name(), classDecl->name);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }
  if (!isRelated) {
    auto message = ErrorTextInstanceOf::neverMade(base->name(), classDecl->name);

    return err(CodeClassesRegistryCheckInstanceOfError{
        .code = CodeClassesRegistryCheckInstanceOfErrorCode::NEVER_CREATED,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecks::ResultBytesKindTested InstanceOfChecks::checkBytesInstanceOf(BinaryExpr *binary,
                                                                               const Type *bytes) {
  const Type *type = binary->lhs->type;
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(bytes->name());
  binary->rhs->type = bytes;
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(binary);
  }
  if (base->isUnion()) {
    return this->checkBytesUnion(binary, bytes);
  }
  if (this->isBytesKind(bytes, type)) {
    auto message = ErrorTextInstanceOf::alwaysTrue(type->name(), bytes->name());

    return err(CodeClassesRegistryCheckBytesInstanceOfError{
        .code = CodeClassesRegistryCheckBytesInstanceOfErrorCode::ALWAYS_TRUE,
        .message = message,
    });
  }
  if (type->isOptional() && this->isBytesKind(bytes, type->inner())) {
    auto message = ErrorTextInstanceOf::orUndef(type->inner()->name());

    return err(CodeClassesRegistryCheckBytesInstanceOfError{
        .code = CodeClassesRegistryCheckBytesInstanceOfErrorCode::TEST_UNDEFINED,
        .message = message,
    });
  }
  if (!this->canBeBufferOf(bytes, base)) {
    auto message = ErrorTextInstanceOf::neverIs(type->name(), what, bytes->name());

    return err(CodeClassesRegistryCheckBytesInstanceOfError{
        .code = CodeClassesRegistryCheckBytesInstanceOfErrorCode::ALWAYS_FALSE,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecks::ResultBytesKindTested InstanceOfChecks::checkBytesUnion(BinaryExpr *binary,
                                                                          const Type *bytes) {
  const Type *type = binary->lhs->type;
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(bytes->name());
  auto matching = std::vector<const Type *>();
  bool isMaybe = false;
  for (const Type *member : base->members()) {
    if (this->isBytesKind(bytes, member)) {
      matching.push_back(member);
    }
    isMaybe = isMaybe || this->canBeBufferOf(bytes, member);
  }
  if (matching.empty() && !isMaybe) {
    auto message = ErrorTextInstanceOf::noneIs(base->name(), what);

    return err(CodeClassesRegistryCheckBytesInstanceOfError{
        .code = CodeClassesRegistryCheckBytesInstanceOfErrorCode::NONE_MATCHES,
        .message = message,
    });
  }
  if (matching.size() == base->members().size() && !type->isOptional()) {
    auto message = ErrorTextInstanceOf::allAre(base->name(), what, bytes->name());

    return err(CodeClassesRegistryCheckBytesInstanceOfError{
        .code = CodeClassesRegistryCheckBytesInstanceOfErrorCode::ALL_MATCH,
        .message = message,
    });
  }
  if (!bytes->isBuffer()) {
    binary->rhs->type = Type::unionOf(matching);
  }

  return ok(binary);
}

bool InstanceOfChecks::isBytesKind(const Type *bytes, const Type *candidate) const {
  bool isBufferOfUint8 = bytes->bytes() == Type::Bytes::Uint8 && candidate->isBuffer();

  return candidate->isBytes() && (candidate->bytes() == bytes->bytes() || isBufferOfUint8);
}

bool InstanceOfChecks::canBeBufferOf(const Type *bytes, const Type *candidate) const {
  return bytes->isBuffer() && candidate->bytes() == Type::Bytes::Uint8;
}

}
