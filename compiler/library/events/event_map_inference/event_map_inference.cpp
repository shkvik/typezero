#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// The file of a place that placeOf gives: what comes before its line and
// column.
std::string fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

// Whether the next round may take `type` as it is: it has no class in it,
// nor a type that its declaration alone makes (a recursive one), which
// each round declares anew.
bool sameInEveryRound(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
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
    return llvm::all_of(types, [&](const Type *t) { return !t || sameInEveryRound(t, seen); });
  };
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return sameInEveryRound(type->inner(), seen);
  case Type::Kind::Function:
    return all(type->params()) && sameInEveryRound(type->ret(), seen);
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
      if (!sameInEveryRound(f.type, seen)) {
        return false;
      }
    }
    return !type->tupleRest() || sameInEveryRound(type->tupleRest(), seen);
  default:
    return true;
  }
}

} // namespace

EventMapInference::EventMapInference(Checker &checker) : checker_(checker) {}

std::string EventMapInference::placeOf(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = checker_.diags_.sourceMgr();
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
  using Kind = EventInference::Portable::Kind;
  EventInference::Portable p;
  llvm::SmallPtrSet<const Type *, 8> visited;
  if (sameInEveryRound(type, visited)) {
    p.same = type;
    return p;
  }
  if (!seen.insert(type).second) {
    p.kind = Kind::Bad; // a recursive type
    return p;
  }
  auto part = [&](const Type *t) { p.parts.push_back(portable(t, seen)); };
  if (type->isClass() || type->isClassRef()) {
    const ClassDecl *cls = type->isClass() ? type->classDecl() : type->classRef();
    if (cls->literal || cls->name.empty()) {
      p.kind = Kind::Bad;
      return p;
    }
    p.kind = type->isClass() ? Kind::Class : Kind::ClassRef;
    const ClassDecl *declared = cls->generic ? cls->generic : cls;
    p.name = declared->name.str();
    if (!declared->builtin) {
      p.file = fileOfPlace(placeOf(declared->loc()));
    }
    if (type->isClass() && cls->generic) {
      for (const Type *arg : cls->typeArgs) {
        part(arg);
      }
    }
    return p;
  }
  switch (type->kind()) {
  case Type::Kind::Optional:
    p.kind = Kind::Optional;
    part(type->inner());
    break;
  case Type::Kind::Array:
    p.kind = Kind::Array;
    part(type->inner());
    break;
  case Type::Kind::Promise:
    p.kind = Kind::Promise;
    part(type->inner());
    break;
  case Type::Kind::Function:
    p.kind = Kind::Function;
    for (const Type *t : type->params()) {
      part(t);
    }
    part(type->ret());
    p.min = type->minArgs();
    p.rest = type->hasRest();
    break;
  case Type::Kind::Union:
    p.kind = Kind::Union;
    for (const Type *t : type->members()) {
      part(t);
    }
    break;
  case Type::Kind::Dict:
    p.kind = Kind::Dict;
    p.dict = int(type->dictKind());
    part(type->keyType());
    part(type->valueType());
    break;
  case Type::Kind::Iterator:
    p.kind = Kind::Iterator;
    part(type->yielded());
    part(type->returned());
    part(type->sent());
    break;
  case Type::Kind::Result:
    p.kind = Kind::Result;
    part(type->okType());
    part(type->errType());
    break;
  case Type::Kind::Object:
    if (type->isTuple()) {
      p.kind = Kind::Tuple;
      for (const Field &f : type->fields()) {
        part(f.type);
      }
      p.min = type->tupleMin();
      p.rest = type->tupleRest() != nullptr;
      if (p.rest) {
        part(type->tupleRest());
      }
      break;
    }
    p.kind = Kind::Object;
    for (const Field &f : type->fields()) {
      p.names.push_back(f.name);
      p.readonly.push_back(f.readonly);
      part(f.type);
    }
    break;
  default:
    p.kind = Kind::Bad;
    break;
  }
  seen.erase(type);
  for (const EventInference::Portable &q : p.parts) {
    if (q.kind == Kind::Bad) {
      p.kind = Kind::Bad;
    }
  }

  return p;
}

const Type *EventMapInference::fromPortable(const EventInference::Portable &p, llvm::SMLoc loc) {
  using Kind = EventInference::Portable::Kind;
  std::vector<const Type *> parts;
  for (const EventInference::Portable &q : p.parts) {
    parts.push_back(fromPortable(q, loc));
    if (!parts.back()) {
      return nullptr;
    }
  }
  switch (p.kind) {
  case Kind::Same:
    return p.same;
  case Kind::Bad:
    return nullptr;
  case Kind::Class:
  case Kind::ClassRef: {
    ClassDecl *cls = nullptr;
    if (p.file.empty()) {
      cls = checker_.throwChecker().errorKindClass(p.name);
      if (!cls && checker_.systemErrorClass_ && p.name == checker_.systemErrorClass_->name) {
        cls = checker_.systemErrorClass_;
      }
      if (!cls && checker_.statsClass_ && p.name == checker_.statsClass_->name) {
        cls = checker_.statsClass_;
      }
      if (!cls && checker_.direntClass_ && p.name == checker_.direntClass_->name) {
        cls = checker_.direntClass_;
      }
    } else {
      for (Module *module : checker_.file_->modules) {
        for (Stmt *item : module->file.items) {
          if (auto *c = dyn_cast<ClassDecl>(item);
              c && !cls && c->name == p.name && fileOfPlace(placeOf(c->loc())) == p.file) {
            cls = c;
          }
        }
      }
    }
    if (!cls || !cls->type) {
      return nullptr;
    }
    if (p.kind == Kind::ClassRef) {
      return Type::classRef(cls, cls->name);
    }
    if (cls->isGeneric()) {
      ClassDecl *inst = checker_.genericClassInstantiator().instantiateClass(cls, parts, loc);
      return inst ? inst->type : nullptr;
    }
    return cls->type;
  }
  case Kind::Optional:
    return Type::optional(parts[0]);
  case Kind::Array:
    return Type::array(parts[0]);
  case Kind::Promise:
    return Type::promise(parts[0]);
  case Kind::Function: {
    const Type *ret = parts.back();
    parts.pop_back();
    return Type::function(parts, p.min, p.rest, ret);
  }
  case Kind::Union:
    return Type::unionOf(parts);
  case Kind::Dict:
    return Type::dict(Type::DictKind(p.dict), parts[0], parts[1]);
  case Kind::Iterator:
    return Type::iterator(parts[0], parts[1], parts[2]);
  case Kind::Result:
    return Type::result(parts[0], parts[1]);
  case Kind::Tuple: {
    const Type *rest = nullptr;
    if (p.rest) {
      rest = parts.back();
      parts.pop_back();
    }
    return Type::argumentList(parts, p.min, rest);
  }
  case Kind::Object: {
    std::vector<Field> fields;
    for (size_t i = 0; i < parts.size(); ++i) {
      fields.push_back({p.names[i], parts[i], p.readonly[i]});
    }
    return Type::object(std::move(fields));
  }
  }

  return nullptr;
}

const Type *EventMapInference::inferredEventMap(const std::string &site) {
  if (auto found = siteMaps_.find(site); found != siteMaps_.end()) {
    return found->second;
  }
  if (!checker_.inference_) {
    return nullptr;
  }
  checker_.inference_->open = true;
  const Type *map;
  if (!checker_.inference_->final) {
    // Opaque, so that the calls on its emitters can tell it is this one.
    Type *opaque = Type::declareObject("the events of " + site);
    opaque->defineFields({});
    checker_.openMaps_[opaque] = site;
    map = opaque;
  } else {
    std::vector<Field> fields;
    for (const EventInference::Event &e : checker_.inference_->maps[site]) {
      if (const Type *args = fromPortable(e.args, llvm::SMLoc())) {
        fields.push_back({e.name, args, false});
      }
    }
    map = Type::object(std::move(fields));
    checker_.inferredMaps_[map] = site;
  }
  siteMaps_[site] = map;

  return map;
}

} // namespace tz
