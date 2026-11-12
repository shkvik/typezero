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

bool ChunkTypes::isStreamClass(const ClassDecl *classDecl) const {
  auto *stream = this->state.getLibraryDeclarations().findLibraryClass("Stream");

  return classDecl != nullptr && stream != nullptr && classDecl->derivesFrom(stream);
}

StreamChunks ChunkTypes::streamChunksOf(const ClassDecl *classDecl) const {
  StreamChunks chunks;
  for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
    if (!c->library() || c->generic() == nullptr || c->typeArgs.empty()) {
      continue;
    }
    llvm::StringRef name = c->generic()->name;
    if (name == "Readable" && chunks.readable == nullptr) {
      chunks.readable = c->typeArgs[0];
    } else if (name == "Writable" && chunks.writable == nullptr) {
      chunks.writable = c->typeArgs[0];
    } else if (name == "Duplex" && c->typeArgs.size() >= 2 && chunks.writable == nullptr) {
      chunks.writable = c->typeArgs[0];
    }
  }

  return chunks;
}

void ChunkTypes::addStreamBases(ClassDecl *instance, llvm::SMLoc loc) {
  ClassDecl *generic = instance->generic();
  auto *writable = this->state.getLibraryDeclarations().findLibraryClass("Writable");
  if (generic == nullptr || !generic->library() || generic->name != "Duplex" ||
      writable == nullptr || instance->typeArgs.size() != 3) {
    return;
  }
  auto side = this->genericClassInstantiator->instantiateClass(
      writable, {instance->typeArgs[0], instance->typeArgs[2]}, loc);
  if (side.isErr() &&
      side.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        side.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? writable->loc()
            : (loc.isValid() ? loc : writable->loc());
    this->state.getDiagnostics().error(at, side.error().message);
  }
  if (side.isOk()) {
    this->classBases->addBase(instance->type, side.value()->type);
    if (instance->isEmitted()) {
      this->state.getInventoryStore().addSubtype(instance->type);
    }
  }
}

std::optional<std::vector<const Type *>>
ChunkTypes::streamHookParams(const FunctionDecl *functionDecl) {
  if (!this->isStreamHook(functionDecl, false)) {
    return std::nullopt;
  }
  const auto *lib = this->libraryBase(functionDecl->cls()->parent);
  std::optional<StreamSides> sides = lib != nullptr ? this->streamSides(lib) : std::nullopt;
  if (!sides.has_value()) {
    return std::nullopt;
  }
  auto params = this->hookParamTypes(functionDecl, *sides);
  if (params.empty() || llvm::is_contained(params, nullptr)) {
    return std::nullopt;
  }

  return params;
}

const ClassDecl *ChunkTypes::libraryBase(const ClassDecl *classDecl) const {
  const ClassDecl *lib = classDecl;
  while (lib != nullptr && !lib->library()) {
    lib = lib->parent;
  }
  if (lib == nullptr || lib->generic() == nullptr) {
    return nullptr;
  }

  return lib;
}

std::optional<ChunkTypes::StreamSides> ChunkTypes::streamSides(const ClassDecl *lib) {
  const auto *streamError = this->prelude->builtinType(PreludeType::STREAM_ERROR);
  auto kind = StreamKindTable::find(lib->generic()->name);
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

std::vector<const Type *> ChunkTypes::hookParamTypes(const FunctionDecl *functionDecl,
                                                     const StreamSides &sides) {
  const auto *callback = this->libraryAliasOf("HookCallback", sides.error, functionDecl->loc());
  const auto *transformCallback =
      this->libraryAliasOf("TransformHookCallback", sides.error, functionDecl->loc());
  auto hook = StreamHookTable::find(functionDecl->name);
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
      return this->writevParamTypes(functionDecl, sides, callback);
  }

  return {};
}

std::vector<const Type *> ChunkTypes::destroyParamTypes() {
  const auto *error = this->prelude->builtinType(PreludeType::STREAM_ERROR);
  auto *aliasDecl = this->state.getLibraryDeclarations().findLibraryType("StreamCallback");
  const Type *callback = aliasDecl != nullptr ? aliasDecl->resolved : nullptr;

  return {Type::optional(error), callback};
}

std::vector<const Type *> ChunkTypes::writevParamTypes(const FunctionDecl *functionDecl,
                                                       const StreamSides &sides,
                                                       const Type *callback) {
  if (sides.writable == nullptr) {
    return {};
  }
  const auto *chunk = this->libraryAliasOf("WriteChunk", sides.writable, functionDecl->loc());
  if (chunk == nullptr) {
    return {};
  }

  return {Type::array(chunk), callback};
}

const Type *ChunkTypes::libraryAliasOf(llvm::StringRef name, const Type *arg, llvm::SMLoc loc) {
  auto *aliasDecl = this->state.getLibraryDeclarations().findLibraryType(name);
  if (aliasDecl == nullptr) {
    return nullptr;
  }

  return this->aliasInstantiator->instantiateAlias(aliasDecl, {arg}, loc);
}

bool ChunkTypes::isStreamHook(const FunctionDecl *functionDecl, bool linked) const {
  const ClassDecl *classDecl = functionDecl->cls();

  return classDecl != nullptr && !functionDecl->isStatic() && !classDecl->library() &&
         classDecl->literal() == nullptr && classDecl->parent != nullptr &&
         StreamHookTable::find(functionDecl->name).has_value() &&
         (!linked || this->isStreamClass(classDecl->parent));
}

ChunkTypes::ResultHookParamsMatch
ChunkTypes::hookParams(const FunctionDecl *functionDecl,
                       llvm::ArrayRef<FunctionDecl *> inherited) const {
  const FuncSymbol *hook = nullptr;
  for (const FunctionDecl *v : inherited) {
    if (v->sym != nullptr && v->cls() != nullptr && v->cls()->library()) {
      hook = v->sym;
    }
  }
  if (hook == nullptr) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::NO_LIBRARY_HOOK,
        .message = "",
    });
  }
  auto untyped = [](const Type *type) { return type == nullptr || type->isError(); };
  if (llvm::any_of(functionDecl->sym->params, untyped)) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::PARAMS_NOT_TYPED,
        .message = "",
    });
  }
  bool hasChunks = functionDecl->name == "onWrite" || functionDecl->name == "onWritev" ||
                   functionDecl->name == "onTransform";
  auto params = ErrorTextStreamHook::params(hook);
  auto message = ErrorTextStreamHook::other(functionDecl->name, params, hasChunks);

  return err(ChunkTypesHookParamsError{
      .code = ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS,
      .message = message,
  });
}

}
