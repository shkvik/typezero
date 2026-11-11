#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/program/library/stream/stream_hook_table/stream_hook_table.h"
#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ChunkTypes::ChunkTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->classBases = &all.classBases;
  this->aliasInstantiator = &all.aliasInstantiator;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool ChunkTypes::isStreamClass(const ClassDecl *cls) const {
  ClassDecl *stream = this->state.getLibraryDeclarations().findLibraryClass("Stream");

  return cls && stream && cls->derivesFrom(stream);
}

StreamChunks ChunkTypes::streamChunksOf(const ClassDecl *cls) const {
  StreamChunks chunks;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library() || !c->generic() || c->typeArgs.empty()) {
      continue;
    }
    llvm::StringRef name = c->generic()->name;
    if (name == "Readable" && !chunks.readable) {
      chunks.readable = c->typeArgs[0];
    } else if (name == "Writable" && !chunks.writable) {
      chunks.writable = c->typeArgs[0];
    } else if (name == "Duplex" && c->typeArgs.size() >= 2 && !chunks.writable) {
      chunks.writable = c->typeArgs[0];
    }
  }

  return chunks;
}

void ChunkTypes::addStreamBases(ClassDecl *inst, llvm::SMLoc loc) {
  ClassDecl *generic = inst->generic();
  ClassDecl *writable = this->state.getLibraryDeclarations().findLibraryClass("Writable");
  if (!generic || !generic->library() || generic->name != "Duplex" || !writable ||
      inst->typeArgs.size() != 3) {
    return;
  }
  GenericClassInstantiator::ResultClassMade side = this->genericClassInstantiator->instantiateClass(
      writable, {inst->typeArgs[0], inst->typeArgs[2]}, loc);
  if (side.isErr() &&
      side.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        side.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? writable->loc()
            : (loc.isValid() ? loc : writable->loc());
    this->state.getDiagnostics().error(at, side.error().message);
  }
  if (side.isOk()) {
    this->classBases->addBase(inst->type, side.value()->type);
    if (inst->isEmitted()) {
      this->state.getInventoryStore().addSubtype(inst->type);
    }
  }
}

std::optional<std::vector<const Type *>> ChunkTypes::streamHookParams(const FunctionDecl *fn) {
  if (!this->isStreamHook(fn, false)) {
    return std::nullopt;
  }
  const ClassDecl *lib = ChunkTypes::libraryBase(fn->cls()->parent);
  std::optional<StreamSides> sides = lib != nullptr ? this->streamSides(lib) : std::nullopt;
  if (!sides.has_value()) {
    return std::nullopt;
  }
  std::vector<const Type *> params = this->hookParamTypes(fn, *sides);
  if (params.empty() || llvm::is_contained(params, nullptr)) {
    return std::nullopt;
  }

  return params;
}

const ClassDecl *ChunkTypes::libraryBase(const ClassDecl *cls) {
  const ClassDecl *lib = cls;
  while (lib != nullptr && !lib->library()) {
    lib = lib->parent;
  }
  if (lib == nullptr || lib->generic() == nullptr) {
    return nullptr;
  }

  return lib;
}

std::optional<ChunkTypes::StreamSides> ChunkTypes::streamSides(const ClassDecl *lib) {
  const Type *streamError = this->prelude->builtinType("StreamError");
  std::optional<StreamKind> kind = StreamKindTable::find(lib->generic()->name);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  const std::vector<const Type *> &args = lib->typeArgs;
  switch (*kind) {
    case StreamKind::READABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[0],
          .writable = nullptr,
          .error = args[1],
          .transform = false,
      };
    case StreamKind::WRITABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = nullptr,
          .writable = args[0],
          .error = args[1],
          .transform = false,
      };
    case StreamKind::DUPLEX:
    case StreamKind::TRANSFORM:
      if (args.size() != 3) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[1],
          .writable = args[0],
          .error = args[2],
          .transform = *kind == StreamKind::TRANSFORM,
      };
    case StreamKind::PASS_THROUGH:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[1],
          .writable = args[0],
          .error = streamError,
          .transform = true,
      };
  }

  return std::nullopt;
}

std::vector<const Type *> ChunkTypes::hookParamTypes(const FunctionDecl *fn,
                                                     const StreamSides &sides) {
  const Type *callback = this->libraryAliasOf("HookCallback", sides.error, fn->loc());
  const Type *transformCallback =
      this->libraryAliasOf("TransformHookCallback", sides.error, fn->loc());
  std::optional<StreamHook> hook = StreamHookTable::find(fn->name);
  if (!hook.has_value()) {
    return {};
  }
  switch (*hook) {
    case StreamHook::ON_READ:
      if (sides.readable == nullptr) {
        return {};
      }
      return {Type::intTy()};
    case StreamHook::ON_FINAL:
    case StreamHook::ON_CONSTRUCT:
      return {callback};
    case StreamHook::ON_DESTROY:
      return this->destroyParamTypes();
    case StreamHook::ON_FLUSH:
      if (!sides.transform) {
        return {};
      }
      return {transformCallback};
    case StreamHook::ON_TRANSFORM:
      if (!sides.transform) {
        return {};
      }
      return {sides.writable, Type::string(), transformCallback};
    case StreamHook::ON_WRITE:
      if (sides.writable == nullptr) {
        return {};
      }
      return {sides.writable, Type::string(), callback};
    case StreamHook::ON_WRITEV:
      return this->writevParamTypes(fn, sides, callback);
  }

  return {};
}

std::vector<const Type *> ChunkTypes::destroyParamTypes() {
  const Type *error = this->prelude->builtinType("StreamError");
  TypeAliasDecl *decl = this->state.getLibraryDeclarations().findLibraryType("StreamCallback");
  const Type *callback = decl != nullptr ? decl->resolved : nullptr;

  return {Type::optional(error), callback};
}

std::vector<const Type *> ChunkTypes::writevParamTypes(const FunctionDecl *fn,
                                                       const StreamSides &sides,
                                                       const Type *callback) {
  if (sides.writable == nullptr) {
    return {};
  }
  const Type *chunk = this->libraryAliasOf("WriteChunk", sides.writable, fn->loc());
  if (chunk == nullptr) {
    return {};
  }

  return {Type::array(chunk), callback};
}

const Type *ChunkTypes::libraryAliasOf(llvm::StringRef name, const Type *arg, llvm::SMLoc loc) {
  TypeAliasDecl *decl = this->state.getLibraryDeclarations().findLibraryType(name);
  if (decl == nullptr) {
    return nullptr;
  }

  return this->aliasInstantiator->instantiateAlias(decl, {arg}, loc);
}

bool ChunkTypes::isStreamHook(const FunctionDecl *fn, bool linked) const {
  const ClassDecl *cls = fn->cls();

  return cls && !fn->isStatic() && !cls->library() && !cls->literal() && cls->parent &&
         StreamHookTable::find(fn->name).has_value() &&
         (!linked || this->isStreamClass(cls->parent));
}

ChunkTypes::ResultHookParamsMatch
ChunkTypes::hookParams(const FunctionDecl *fn, llvm::ArrayRef<FunctionDecl *> inherited) const {
  const FuncSymbol *hook = nullptr;
  for (const FunctionDecl *v : inherited) {
    if (v->sym && v->cls() && v->cls()->library()) {
      hook = v->sym;
    }
  }
  if (!hook) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::NO_LIBRARY_HOOK,
        .message = "",
    });
  }
  auto untyped = [](const Type *t) { return !t || t->isError(); };
  if (llvm::any_of(fn->sym->params, untyped)) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::PARAMS_NOT_TYPED,
        .message = "",
    });
  }
  bool hasChunks = fn->name == "onWrite" || fn->name == "onWritev" || fn->name == "onTransform";
  std::string params = ErrorTextStreamHook::params(hook);
  std::string message = ErrorTextStreamHook::other(fn->name, params, hasChunks);

  return err(ChunkTypesHookParamsError{
      .code = ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS,
      .message = message,
  });
}

}
