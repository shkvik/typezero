#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

std::string EventMapInference::fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

bool EventMapInference::sameInEveryRound(const Type *type,
                                         llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!seen.insert(type).second) {
    return true;
  }
  if (type->isClass() || type->isClassRef()) {
    return false;
  }
  if (type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isBytes() &&
      !type->isDate() && !type->isRegExp() && !type->isIntl() && !type->isCrypto() &&
      !type->isRuntimeObject() && !type->isMatch() && !type->isTimer() &&
      Type::object(std::vector<Field>(type->fields().begin(), type->fields().end()), {},
                   std::vector<Overload>(type->overloads().begin(), type->overloads().end())) !=
          type) {
    return false;
  }
  auto all = [&](llvm::ArrayRef<const Type *> types) {
    return llvm::all_of(
        types, [&](const Type *t) { return !t || EventMapInference::sameInEveryRound(t, seen); });
  };
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return EventMapInference::sameInEveryRound(type->inner(), seen);
  case Type::Kind::Function:
    return all(type->params()) && EventMapInference::sameInEveryRound(type->ret(), seen);
  case Type::Kind::Union:
    return all(type->members());
  case Type::Kind::Dict:
    return all({type->keyType(), type->valueType()});
  case Type::Kind::Iterator:
    return all({type->yielded(), type->returned(), type->sent()});
  case Type::Kind::Result:
    return all({type->okType(), type->errType()});
  case Type::Kind::Object:
    for (const Field &f : type->fields()) {
      if (!EventMapInference::sameInEveryRound(f.type, seen)) {
        return false;
      }
    }
    return !type->tupleRest() || EventMapInference::sameInEveryRound(type->tupleRest(), seen);
  default:
    return true;
  }
}

EventMapInference::EventMapInference(SemanticState &owner)
    : state(owner) {}

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

EventInference::Portable EventMapInference::portable(const Type *type,
                                                     llvm::SmallPtrSetImpl<const Type *> &seen) {
  EventInference::Portable p;
  llvm::SmallPtrSet<const Type *, 8> visited;
  if (EventMapInference::sameInEveryRound(type, visited)) {
    p.form = PortableSame{.type = type};
    return p;
  }
  if (!seen.insert(type).second) {
    p.form = PortableBad{};
    return p;
  }
  auto part = [&](const Type *t) { p.parts.push_back(this->portable(t, seen)); };
  if (type->isClass() || type->isClassRef()) {
    const ClassDecl *cls = type->isClass() ? type->classDecl() : type->classRef();
    if (cls->literal() || cls->name.empty()) {
      p.form = PortableBad{};
      return p;
    }
    const ClassDecl *declared = cls->generic() ? cls->generic() : cls;
    std::string name = declared->name.str();
    std::string file = declared->builtin()
                           ? std::string()
                           : EventMapInference::fileOfPlace(this->placeOf(declared->loc()));
    if (type->isClassRef()) {
      p.form = PortableClassRef{.file = std::move(file), .name = std::move(name)};
      return p;
    }
    p.form = PortableClass{.file = std::move(file), .name = std::move(name)};
    if (cls->generic()) {
      for (const Type *arg : cls->typeArgs) {
        part(arg);
      }
    }
    return p;
  }
  switch (type->kind()) {
    case Type::Kind::Optional:
      p.form = PortableOptional{};
      part(type->inner());
      break;
    case Type::Kind::Array:
      p.form = PortableArray{};
      part(type->inner());
      break;
    case Type::Kind::Promise:
      p.form = PortablePromise{};
      part(type->inner());
      break;
    case Type::Kind::Function:
      p.form = PortableFunction{.min = type->minArgs(), .rest = type->hasRest()};
      for (const Type *t : type->params()) {
        part(t);
      }
      part(type->ret());
      break;
    case Type::Kind::Union:
      p.form = PortableUnion{};
      for (const Type *t : type->members()) {
        part(t);
      }
      break;
    case Type::Kind::Dict:
      p.form = PortableDict{.dict = int(type->dictKind())};
      part(type->keyType());
      part(type->valueType());
      break;
    case Type::Kind::Iterator:
      p.form = PortableIterator{};
      part(type->yielded());
      part(type->returned());
      part(type->sent());
      break;
    case Type::Kind::Result:
      p.form = PortableResult{};
      part(type->okType());
      part(type->errType());
      break;
    case Type::Kind::Object: {
      if (type->isTuple()) {
        bool rest = type->tupleRest() != nullptr;
        p.form = PortableTuple{.min = type->tupleMin(), .rest = rest};
        for (const Field &f : type->fields()) {
          part(f.type);
        }
        if (rest) {
          part(type->tupleRest());
        }
        break;
      }
      PortableObject object;
      for (const Field &f : type->fields()) {
        object.names.push_back(f.name);
        object.readonly.push_back(f.readonly);
        part(f.type);
      }
      p.form = std::move(object);
      break;
    }
    default:
      p.form = PortableBad{};
      break;
  }
  seen.erase(type);
  for (const EventInference::Portable &q : p.parts) {
    if (q.form.is<PortableBad>()) {
      p.form = PortableBad{};
    }
  }

  return p;
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
  std::vector<const Type *> parts;
  for (const EventInference::Portable &q : p.parts) {
    parts.push_back(this->fromPortable(q, loc));
    if (!parts.back()) {
      return nullptr;
    }
  }
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
          GenericClassInstantiator(this->state).instantiateClass(cls, parts, loc);
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
