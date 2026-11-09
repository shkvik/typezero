#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

EventMapInference::EventMapInference(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->genericClassInstantiator = &all.getGenericClassInstantiator();
}

std::string EventMapInference::fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

bool EventMapInference::sameInEveryRound(const Type *type) {
  TypeParts parts = TypeParts::collect(type, kSameEveryRoundRoute);
  for (const Type *part : parts.getTypes()) {
    if (!EventMapInference::isSameEveryRoundPart(part)) {
      return false;
    }
  }

  return true;
}

bool EventMapInference::isSameEveryRoundPart(const Type *type) {
  if (type->isClass() || type->isClassRef()) {
    return false;
  }
  bool plainObject = type->isObject() && !type->isTuple() && !type->isAnyObject() &&
                     !type->isBytes() && !type->isDate() && !type->isRegExp() && !type->isIntl() &&
                     !type->isCrypto() && !type->isRuntimeObject() && !type->isMatch() &&
                     !type->isTimer();
  if (!plainObject) {
    return true;
  }
  std::vector<Field> fields(type->fields().begin(), type->fields().end());
  std::vector<Overload> overloads(type->overloads().begin(), type->overloads().end());
  const Type *canonical = Type::object(std::move(fields), {}, std::move(overloads));

  return canonical == type;
}

std::string EventMapInference::placeOf(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = this->state.getDiagnostics().sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (!id) {
    return "?";
  }
  auto [line, column] = sm.getLineAndColumn(loc, id);

  return sm.getMemoryBuffer(id)->getBufferIdentifier().str() + ":" + std::to_string(line) + ":" +
         std::to_string(column);
}

EventInference::Portable EventMapInference::portable(const Type *type) {
  llvm::SmallPtrSet<const Type *, 8> seen;
  std::vector<PortableStep> steps = {EnterPortable{type}};
  std::vector<EventInference::Portable> answers;
  while (!steps.empty()) {
    PortableStep step = std::move(steps.back());
    steps.pop_back();
    if (const EnterPortable *enter = step.as<EnterPortable>()) {
      this->enterPortable(enter->type, seen, steps, answers);
    } else {
      EventMapInference::leavePortable(*step.as<LeavePortable>(), seen, answers);
    }
  }

  return answers.back();
}

void EventMapInference::enterPortable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen,
                                      std::vector<PortableStep> &steps,
                                      std::vector<EventInference::Portable> &answers) {
  if (EventMapInference::sameInEveryRound(type)) {
    EventInference::Portable same;
    same.form = PortableSame{.type = type};
    answers.push_back(std::move(same));
  } else if (!seen.insert(type).second) {
    EventInference::Portable bad;
    bad.form = PortableBad{};
    answers.push_back(std::move(bad));
  } else {
    PortableHead head = this->portableHead(type);
    std::vector<const Type *> parts = head.parts;
    steps.push_back(LeavePortable{type, std::move(head)});
    for (const Type *part : llvm::reverse(parts)) {
      steps.push_back(EnterPortable{part});
    }
  }
}

void EventMapInference::leavePortable(const LeavePortable &leave,
                                      llvm::SmallPtrSetImpl<const Type *> &seen,
                                      std::vector<EventInference::Portable> &answers) {
  EventInference::Portable p;
  p.form = leave.head.form;
  p.parts = ChildrenFirst::takeAnswers(answers, leave.head.parts.size());
  if (!leave.head.keepsSeen) {
    seen.erase(leave.type);
    for (const EventInference::Portable &q : p.parts) {
      if (q.form.is<PortableBad>()) {
        p.form = PortableBad{};
      }
    }
  }
  answers.push_back(std::move(p));
}

PortableHead EventMapInference::portableHead(const Type *type) const {
  if (type->isClass() || type->isClassRef()) {
    return this->classHead(type);
  }
  PortableHead head;
  head.keepsSeen = false;
  switch (type->kind()) {
    case Type::Kind::Optional:
      head.form = PortableOptional{};
      head.parts = {type->inner()};
      break;
    case Type::Kind::Array:
      head.form = PortableArray{};
      head.parts = {type->inner()};
      break;
    case Type::Kind::Promise:
      head.form = PortablePromise{};
      head.parts = {type->inner()};
      break;
    case Type::Kind::Function:
      head.form = PortableFunction{.min = type->minArgs(), .rest = type->hasRest()};
      head.parts.assign(type->params().begin(), type->params().end());
      head.parts.push_back(type->ret());
      break;
    case Type::Kind::Union:
      head.form = PortableUnion{};
      head.parts.assign(type->members().begin(), type->members().end());
      break;
    case Type::Kind::Dict:
      head.form = PortableDict{.dict = int(type->dictKind())};
      head.parts = {type->keyType(), type->valueType()};
      break;
    case Type::Kind::Iterator:
      head.form = PortableIterator{};
      head.parts = {type->yielded(), type->returned(), type->sent()};
      break;
    case Type::Kind::Result:
      head.form = PortableResult{};
      head.parts = {type->okType(), type->errType()};
      break;
    case Type::Kind::Object:
      return type->isTuple() ? EventMapInference::tupleHead(type)
                             : EventMapInference::objectHead(type);
    default:
      head.form = PortableBad{};
      break;
  }

  return head;
}

PortableHead EventMapInference::classHead(const Type *type) const {
  PortableHead head;
  head.keepsSeen = true;
  const ClassDecl *cls = type->isClass() ? type->classDecl() : type->classRef();
  if (cls->literal() || cls->name.empty()) {
    head.form = PortableBad{};
    return head;
  }
  const ClassDecl *declared = cls->generic() ? cls->generic() : cls;
  std::string name = declared->name.str();
  std::string file = declared->builtin()
                         ? std::string()
                         : EventMapInference::fileOfPlace(this->placeOf(declared->loc()));
  if (type->isClassRef()) {
    head.form = PortableClassRef{.file = std::move(file), .name = std::move(name)};
    return head;
  }
  head.form = PortableClass{.file = std::move(file), .name = std::move(name)};
  if (cls->generic()) {
    head.parts.assign(cls->typeArgs.begin(), cls->typeArgs.end());
  }

  return head;
}

PortableHead EventMapInference::tupleHead(const Type *type) {
  PortableHead head;
  head.keepsSeen = false;
  bool rest = type->tupleRest() != nullptr;
  head.form = PortableTuple{.min = type->tupleMin(), .rest = rest};
  for (const Field &f : type->fields()) {
    head.parts.push_back(f.type);
  }
  if (rest) {
    head.parts.push_back(type->tupleRest());
  }

  return head;
}

PortableHead EventMapInference::objectHead(const Type *type) {
  PortableHead head;
  head.keepsSeen = false;
  PortableObject object;
  for (const Field &f : type->fields()) {
    object.names.push_back(f.name);
    object.readonly.push_back(f.readonly);
    head.parts.push_back(f.type);
  }
  head.form = std::move(object);

  return head;
}

ClassDecl *EventMapInference::portableClass(const std::string &file, const std::string &name) {
  ClassDecl *cls = nullptr;
  if (file.empty()) {
    cls = this->state.getFileSystemStore().findClassNamed(name);
  } else {
    for (Module *module : this->state.getInventoryStore().getModules()) {
      for (Stmt *item : module->file.items) {
        if (auto *c = dyn_cast<ClassDecl>(item);
            c && !cls && c->name == name &&
            EventMapInference::fileOfPlace(this->placeOf(c->loc())) == file) {
          cls = c;
        }
      }
    }
  }

  return cls && cls->type ? cls : nullptr;
}

const Type *EventMapInference::fromPortable(const EventInference::Portable &p, llvm::SMLoc loc) {
  std::vector<const Type *> answers;
  for (const EventInference::Portable *node :
       ChildrenFirst::order(&p, EventMapInference::portableParts)) {
    const Type *type = this->nodeFromPortable(*node, answers, loc);
    if (type == nullptr) {
      return nullptr;
    }
    answers.push_back(type);
  }

  return answers.back();
}

NodeChildren<const EventInference::Portable *>
EventMapInference::portableParts(const EventInference::Portable *p) {
  NodeChildren<const EventInference::Portable *> parts;
  for (const EventInference::Portable &q : p->parts) {
    parts.push_back(&q);
  }

  return parts;
}

const Type *EventMapInference::nodeFromPortable(const EventInference::Portable &p,
                                                std::vector<const Type *> &answers,
                                                llvm::SMLoc loc) {
  std::vector<const Type *> parts = ChildrenFirst::takeAnswers(answers, p.parts.size());
  const Type *type = nullptr;
  switch (p.form.kind()) {
    case PortableKind::SAME_TYPE:
      type = p.form.as<PortableSame>()->type;
      break;
    case PortableKind::BAD_TYPE:
      break;
    case PortableKind::CLASS_TYPE: {
      const PortableClass *named = p.form.as<PortableClass>();
      ClassDecl *cls = this->portableClass(named->file, named->name);
      if (!cls) {
        break;
      }
      if (!cls->isGeneric()) {
        type = cls->type;
        break;
      }
      GenericClassInstantiator::ResultClassMade made =
          this->genericClassInstantiator->instantiateClass(cls, parts, loc);
      if (made.isErr() && made.error().code !=
                              GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
        llvm::SMLoc at =
            made.error().code ==
                    GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                ? cls->loc()
                : (loc.isValid() ? loc : cls->loc());
        this->state.getDiagnostics().error(at, made.error().message);
      }
      type = made.isOk() ? made.value()->type : nullptr;
      break;
    }
    case PortableKind::CLASS_REF_TYPE: {
      const PortableClassRef *named = p.form.as<PortableClassRef>();
      ClassDecl *cls = this->portableClass(named->file, named->name);
      type = cls ? Type::classRef(cls, cls->name) : nullptr;
      break;
    }
    case PortableKind::OPTIONAL_TYPE:
      type = Type::optional(parts[0]);
      break;
    case PortableKind::ARRAY_TYPE:
      type = Type::array(parts[0]);
      break;
    case PortableKind::PROMISE_TYPE:
      type = Type::promise(parts[0]);
      break;
    case PortableKind::FUNCTION_TYPE: {
      const PortableFunction *function = p.form.as<PortableFunction>();
      const Type *ret = parts.back();
      parts.pop_back();
      type = Type::function(parts, function->min, function->rest, ret);
      break;
    }
    case PortableKind::UNION_TYPE:
      type = Type::unionOf(parts);
      break;
    case PortableKind::DICT_TYPE: {
      Type::DictKind dictKind = Type::DictKind(p.form.as<PortableDict>()->dict);
      type = Type::dict(dictKind, parts[0], parts[1]);
      break;
    }
    case PortableKind::ITERATOR_TYPE:
      type = Type::iterator(parts[0], parts[1], parts[2]);
      break;
    case PortableKind::RESULT_TYPE:
      type = Type::result(parts[0], parts[1]);
      break;
    case PortableKind::TUPLE_TYPE: {
      const PortableTuple *tuple = p.form.as<PortableTuple>();
      const Type *rest = nullptr;
      if (tuple->rest) {
        rest = parts.back();
        parts.pop_back();
      }
      type = Type::argumentList(parts, tuple->min, rest);
      break;
    }
    case PortableKind::OBJECT_TYPE: {
      const PortableObject *object = p.form.as<PortableObject>();
      std::vector<Field> fields;
      for (size_t i = 0; i < parts.size(); ++i) {
        fields.push_back({object->names[i], parts[i], object->readonly[i]});
      }
      type = Type::object(std::move(fields));
      break;
    }
  }

  return type;
}

const Type *EventMapInference::inferredEventMap(const std::string &site) {
  EventStore &store = this->state.getEventStore();
  if (const Type *known = store.findSiteMap(site)) {
    return known;
  }
  store.markInferenceOpen();
  const Type *map;
  if (!store.isInferenceFinal()) {
    Type *opaque = Type::declareObject("the events of " + site);
    opaque->defineFields({});
    store.rememberOpenMap(opaque, site);
    map = opaque;
  } else {
    std::vector<Field> fields;
    for (const EventInference::Event &e : store.getInferredEvents(site)) {
      if (const Type *args = this->fromPortable(e.args, llvm::SMLoc())) {
        fields.push_back({e.name, args, false});
      }
    }
    map = Type::object(std::move(fields));
    store.rememberInferredMap(map, site);
  }
  store.rememberSiteMap(site, map);

  return map;
}

}
