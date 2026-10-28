#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ChunkTypes::ChunkTypes(Checker &checker) : checker_(checker) {}

bool ChunkTypes::isStreamClass(const ClassDecl *cls) const {
  ClassDecl *stream = checker_.libraryClass("Stream");

  return cls && stream && cls->derivesFrom(stream);
}

Checker::StreamChunks ChunkTypes::streamChunksOf(const ClassDecl *cls) const {
  Checker::StreamChunks chunks;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library || !c->generic || c->typeArgs.empty()) {
      continue;
    }
    llvm::StringRef name = c->generic->name;
    if (name == "Readable" && !chunks.readable) {
      chunks.readable = c->typeArgs[0];
    } else if (name == "Writable" && !chunks.writable) {
      chunks.writable = c->typeArgs[0];
    } else if (name == "Duplex" && c->typeArgs.size() == 2 && !chunks.writable) {
      chunks.writable = c->typeArgs[0];
    }
  }

  return chunks;
}

void ChunkTypes::addStreamBases(ClassDecl *inst, llvm::SMLoc loc) {
  ClassDecl *generic = inst->generic;
  ClassDecl *writable = checker_.libraryClass("Writable");
  if (!generic || !generic->library || generic->name != "Duplex" || !writable ||
      inst->typeArgs.size() != 2) {
    return;
  }
  if (ClassDecl *side = checker_.genericClassInstantiator().instantiateClass(
          writable, {inst->typeArgs[0]}, loc)) {
    checker_.classBases().addBase(inst->type, side->type);
    if (inst->isEmitted() && !llvm::is_contained(checker_.file_->subtypes, inst->type)) {
      checker_.file_->subtypes.push_back(inst->type);
    }
  }
}

std::optional<std::vector<const Type *>> ChunkTypes::streamHookParams(const FunctionDecl *fn) {
  if (!isStreamHook(fn, false)) {
    return std::nullopt;
  }
  const ClassDecl *lib = fn->cls->parent;
  while (lib && !lib->library) {
    lib = lib->parent;
  }
  if (!lib || !lib->generic) {
    return std::nullopt;
  }
  llvm::StringRef kind = lib->generic->name;
  bool transform = kind == "Transform" || kind == "PassThrough";
  const Type *readable = nullptr, *writable = nullptr;
  if (kind == "Readable" && lib->typeArgs.size() == 1) {
    readable = lib->typeArgs[0];
  } else if (kind == "Writable" && lib->typeArgs.size() == 1) {
    writable = lib->typeArgs[0];
  } else if ((kind == "Duplex" || transform) && lib->typeArgs.size() == 2) {
    writable = lib->typeArgs[0];
    readable = lib->typeArgs[1];
  } else {
    return std::nullopt;
  }
  auto alias = [&](llvm::StringRef name) -> const Type * {
    TypeAliasDecl *decl = checker_.libTypes_.lookup(name);

    return decl ? decl->resolved : nullptr;
  };
  const Type *callback = alias("StreamCallback");
  llvm::StringRef name = fn->name;
  std::vector<const Type *> params;
  if (name == "onRead" && readable) {
    params = {Type::intTy()};
  } else if (name == "onFinal" || name == "onConstruct") {
    params = {callback};
  } else if (name == "onDestroy") {
    params = {Type::optional(checker_.prelude().builtinType("StreamError")), callback};
  } else if (name == "onFlush" && transform) {
    params = {alias("TransformCallback")};
  } else if (name == "onTransform" && transform) {
    params = {writable, Type::string(), alias("TransformCallback")};
  } else if (name == "onWrite" && writable) {
    params = {writable, Type::string(), callback};
  } else if (name == "onWritev" && writable) {
    TypeAliasDecl *entry = checker_.libTypes_.lookup("WriteChunk");
    if (const Type *type =
            entry ? checker_.aliasInstantiator().instantiateAlias(entry, {writable}, fn->loc())
                  : nullptr) {
      params = {Type::array(type), callback};
    }
  }
  if (params.empty() || llvm::is_contained(params, nullptr)) {
    return std::nullopt;
  }

  return params;
}

bool ChunkTypes::isStreamHook(const FunctionDecl *fn, bool linked) const {
  static const llvm::StringLiteral hooks[] = {"onRead",  "onWrite", "onWritev",  "onTransform",
                                              "onFlush", "onFinal", "onDestroy", "onConstruct"};
  const ClassDecl *cls = fn->cls;

  return cls && !fn->isStatic && !cls->library && !cls->literal && cls->parent &&
         llvm::is_contained(hooks, fn->name) && (!linked || isStreamClass(cls->parent));
}

void ChunkTypes::reportStreamHook(const FunctionDecl *fn,
                                  llvm::ArrayRef<FunctionDecl *> inherited) {
  const FuncSymbol *hook = nullptr;
  for (const FunctionDecl *v : inherited) {
    if (v->sym && v->cls && v->cls->library) {
      hook = v->sym;
    }
  }
  if (!hook) {
    return;
  }
  for (const Type *t : fn->sym->params) {
    if (!t || t->isError()) {
      return;
    }
  }
  std::string params;
  for (size_t i = 0; i < hook->params.size(); ++i) {
    if (i) {
      params += ", ";
    }
    llvm::StringRef name = hook->decl && i < hook->decl->params.size() ? hook->decl->params[i].name
                                                                       : llvm::StringRef("arg");
    params += name.str() + ": " + hook->params[i]->name().str();
  }
  std::string chunks;
  if (fn->name == "onWrite" || fn->name == "onWritev" || fn->name == "onTransform") {
    chunks = "; for chunks of another type, give the class it extends type arguments, e.g. "
             "'extends Writable<string>'";
  }
  checker_.diags_.error(
      fn->loc(), "'" + fn->name + "' is what node:stream calls, as '" + fn->name + "(" + params +
                     ")', and a method with other parameters is another "
                     "variant of the name, which it never calls; take those parameters, "
                     "or leave out their types" +
                     chunks);
}

const Type *ChunkTypes::inferredChunkType(const std::string &site, llvm::SMLoc loc) {
  const Type *map = checker_.eventMapInference().inferredEventMap(site);
  if (!map) {
    return nullptr;
  }
  if (checker_.openMaps_.count(map)) {
    return map;
  }
  const Field *chunk = map->field("chunk");
  if (!chunk || chunk->type->fields().empty()) {
    return nullptr;
  }
  (void)loc;

  return chunk->type->fields()[0].type;
}

void ChunkTypes::noteStreamCallback(CallExpr *call, const VarSymbol *var) {
  for (const Checker::StreamCallbackSite &site : checker_.streamCallbacks_) {
    if (site.fn != var->owner || site.param != var->name || call->args.size() < 2) {
      continue;
    }
    const Expr *data = call->args[1];
    while (auto *conv = dyn_cast<CastExpr>(data)) {
      if (!conv->isImplicit()) {
        break;
      }
      data = conv->operand;
    }
    if (!isa<UndefinedExpr>(data)) {
      recordStreamChunk(site.chunk, data->type, data->loc());
    }
  }
}

bool ChunkTypes::recordStreamChunk(const Type *chunk, const Type *value, llvm::SMLoc loc) {
  auto open = checker_.openMaps_.find(chunk);
  if (open == checker_.openMaps_.end()) {
    return false;
  }
  if (value->isError() || value->isVoid() || value->isUndefined() || mentionsParam(value)) {
    return true;
  }
  if (value->isLiteral()) {
    value = value->literalBase();
  }
  value = value->nonOptional();
  const Type *found = Type::tuple({value});
  std::vector<EventInference::Event> &events = checker_.inference_->maps[open->second];
  for (EventInference::Event &e : events) {
    if (e.name != "chunk") {
      continue;
    }
    const Type *before = checker_.eventMapInference().fromPortable(e.args, loc);
    if (!before || before == found || before->fields().size() != 1) {
      return true;
    }
    const Type *both = TypeRelations::unify(before->fields()[0].type, value);
    if (!both) {
      return true;
    }
    if (const Type *now = Type::tuple({both}); now != before) {
      llvm::SmallPtrSet<const Type *, 8> seen;
      e.args = checker_.eventMapInference().portable(now, seen);
      checker_.inference_->changed = true;
    }
    return true;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  events.push_back({"chunk", checker_.eventMapInference().portable(found, seen),
                    checker_.eventMapInference().placeOf(loc)});
  checker_.inference_->changed = true;

  return true;
}

}
