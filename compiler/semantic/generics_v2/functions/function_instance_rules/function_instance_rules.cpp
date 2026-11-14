#include "compiler/semantic/generics_v2/functions/function_instance_rules/function_instance_rules.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/types/type_parts/type_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"

namespace tz {

namespace {

constexpr unsigned kMaxInstanceChain = 16;

}

FunctionInstanceRules::FunctionInstanceRules(SemanticState &owner)
    : state(owner) {}

void FunctionInstanceRules::startInstantiate(InstantiateDraft draft) {
  if (this->hasErrorArg(draft.args)) {
    return;
  }
  FuncSymbol *generic = draft.generic;
  draft.reps = this->instanceReps(generic, draft.args);
  auto keyTypes = this->instanceKey(draft.reps, draft.exact);
  auto &copies = this->state.generics.copies;
  auto *known = copies.findFunctionInstance(generic, keyTypes);
  if (known != nullptr) {
    draft.instance = known;
    bool isWalkDue = copies.isCheckingInstance() && !known->checked && !known->checking;
    auto &tasks = this->state.walk.tasks;
    tasks.pushTask(AfterReusedWalk{.draft = std::move(draft)});
    if (!isWalkDue) {
      return;
    }

    return tasks.pushTask(WalkInstance{.instance = known});
  }
  auto bounded = this->chainBounded(generic, draft.args);
  if (bounded.isErr()) {
    return this->state.diagnostics.error(draft.loc, bounded.error().message);
  }

  return this->state.walk.tasks.pushTask(NextMeaningfulParam{.draft = std::move(draft)});
}

bool FunctionInstanceRules::isMentioningInObject(const Type *type,
                                                 llvm::ArrayRef<const Type *> params) const {
  auto parts = TypeParts::collect(type, kMentionsInObjectRoute);
  for (const Type *part : parts.getTypes()) {
    bool isOperation = part->isParam() && part->typeOp() != TypeOp::None;
    if ((isOperation || part->isObject()) && GenericCheckHelpers::mentionsAnyOf(part, params)) {
      return true;
    }
  }

  return false;
}

const Type *FunctionInstanceRules::representation(const Type *type) const {
  auto answers = std::vector<const Type *>();
  for (const Type *node :
       ChildrenFirst::order(type, this, &FunctionInstanceRules::representationParts)) {
    const auto *shown = this->nodeRepresentation(node, answers);
    answers.push_back(shown);
  }

  return answers.back();
}

NodeChildren<const Type *> FunctionInstanceRules::representationParts(const Type *type) const {
  if (type->kind() == Type::Kind::Array || type->kind() == Type::Kind::Optional) {
    return {type->inner()};
  }
  if (type->kind() == Type::Kind::Result) {
    return {type->okType(), type->errType()};
  }
  if (type->kind() == Type::Kind::Union) {
    return NodeChildren<const Type *>(type->members().begin(), type->members().end());
  }

  return {};
}

const Type *FunctionInstanceRules::nodeRepresentation(const Type *type,
                                                      std::vector<const Type *> &answers) const {
  switch (type->kind()) {
    case Type::Kind::Object:
    case Type::Kind::Iterator:
    case Type::Kind::Dict:
      return Type::anyObject();
    case Type::Kind::Function:
      return Type::function({}, 0, false, Type::voidTy());
    case Type::Kind::Array:
      return Type::array(ChildrenFirst::takeAnswer(answers));
    case Type::Kind::Optional:
      return Type::optional(ChildrenFirst::takeAnswer(answers));
    case Type::Kind::Result:
      return this->resultRepresentation(answers);
    case Type::Kind::Union:
      return this->unionRepresentation(type, answers);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Param:
    case Type::Kind::Never:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      return type;
  }

  return type;
}

const Type *FunctionInstanceRules::resultRepresentation(std::vector<const Type *> &answers) const {
  const Type *errType = ChildrenFirst::takeAnswer(answers);
  const Type *okType = ChildrenFirst::takeAnswer(answers);

  return Type::result(okType, errType);
}

const Type *FunctionInstanceRules::unionRepresentation(const Type *type,
                                                       std::vector<const Type *> &answers) const {
  auto members = ChildrenFirst::takeAnswers(answers, type->members().size());

  return Type::unionOf(members);
}

bool FunctionInstanceRules::isObjectLike(const Type *type) const {
  return type != nullptr && type->nonOptional()->isObject();
}

bool FunctionInstanceRules::hasObjectLikeAlternative(const Type *param) const {
  for (const Type *alternative : param->alternatives()) {
    if (this->isObjectLike(alternative)) {
      return true;
    }
  }

  return false;
}

bool FunctionInstanceRules::isExactParam(const FuncSymbol *generic, size_t index) const {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  const Type *param = params[index];
  if (this->isObjectLike(param->constraint()) || this->hasObjectLikeAlternative(param)) {
    return true;
  }
  if (param->iterated() != nullptr || param->isListParam()) {
    return true;
  }
  for (const Type *other : params) {
    const Type *iterated = other->iterated();
    if (iterated != nullptr && GenericCheckHelpers::mentionsAnyOf(iterated, param)) {
      return true;
    }
  }
  for (const Type *other : params) {
    const Type *constraint = other->constraint();
    bool isOther = other != param && constraint != nullptr;
    if (isOther && GenericCheckHelpers::mentionsAnyOf(constraint, param)) {
      return true;
    }
  }
  for (const Type *type : generic->params) {
    if (type != nullptr && this->isMentioningInObject(type, param)) {
      return true;
    }
  }

  return generic->hasReturnType() &&
         this->isMentioningInObject(generic->getKnownReturnType(), param);
}

std::vector<bool> FunctionInstanceRules::exactParams(const FuncSymbol *generic) const {
  auto exact = std::vector<bool>();
  for (size_t index = 0; index < generic->typeParams.size(); ++index) {
    exact.push_back(this->isExactParam(generic, index));
  }

  return exact;
}

bool FunctionInstanceRules::isExactTypeParam(FuncSymbol *generic, size_t index) {
  auto found = this->state.generics.parameters.findExactParams(generic);
  if (!found.has_value()) {
    auto exact = this->exactParams(generic);
    this->state.generics.parameters.rememberExactParams(generic, exact);
    found = std::move(exact);
  }

  return index < found->size() && (*found)[index];
}

FunctionInstanceRules::ResultChainIsFinite
FunctionInstanceRules::chainBounded(const FuncSymbol *generic,
                                    llvm::ArrayRef<const Type *> args) const {
  unsigned chain = this->chainLength(generic);
  if (chain >= kMaxInstanceChain) {
    auto types = GenericCheckHelpers::typeNames(args, ", ");
    auto message = ErrorTextInstanceChain::endless(generic->name, types);

    return err(GenericsFunctionsRegistryChainBoundedError{
        .code = GenericsFunctionsRegistryChainBoundedErrorCode::ENDLESS_CHAIN,
        .message = message,
    });
  }

  return ok(chain);
}

unsigned FunctionInstanceRules::chainLength(const FuncSymbol *generic) const {
  unsigned chain = 0;
  const InstanceRecord *record = this->state.generics.copies.getCheckingInstance();
  while (record != nullptr) {
    if (record->generic == generic) {
      chain += 1;
    }
    record = record->requester;
  }

  return chain;
}

std::vector<const Type *>
FunctionInstanceRules::instanceKey(llvm::ArrayRef<const Type *> reps,
                                   llvm::ArrayRef<const Type *> exact) const {
  std::vector<const Type *> key(reps.begin(), reps.end());
  if (!exact.empty()) {
    key.push_back(nullptr);
    key.insert(key.end(), exact.begin(), exact.end());
  }

  return key;
}

bool FunctionInstanceRules::hasErrorArg(llvm::ArrayRef<const Type *> args) const {
  for (const Type *type : args) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

std::vector<const Type *> FunctionInstanceRules::instanceReps(FuncSymbol *generic,
                                                              llvm::ArrayRef<const Type *> args) {
  auto reps = std::vector<const Type *>();
  for (size_t index = 0; index < args.size(); ++index) {
    bool isExactParam = this->isExactTypeParam(generic, index);
    const Type *rep = isExactParam ? args[index] : this->representation(args[index]);
    reps.push_back(rep);
  }

  return reps;
}

}
