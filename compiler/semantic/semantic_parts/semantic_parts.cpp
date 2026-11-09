#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

SemanticParts::SemanticParts(SemanticState &state)
    : scopes(state, *this),
      prelude(state, *this),
      fsErrors(state, *this),
      eventMaps(state, *this),
      fsChecker(state, *this),
      osChecker(state, *this),
      byteProofs(state, *this),
      callCycles(state, *this),
      chunkTypes(state, *this),
      classBases(state, *this),
      classLinks(state, *this),
      cryptoArgs(state, *this),
      fieldFacts(state, *this),
      fsDeclarer(state, *this),
      loopBounds(state, *this),
      newChecker(state, *this),
      typeParams(state, *this),
      urlChecker(state, *this),
      varChecker(state, *this),
      callChecker(state, *this),
      dateChecker(state, *this),
      destination(state, *this),
      dictChecker(state, *this),
      dictMethods(state, *this),
      exprChecker(state, *this),
      flowChecker(state, *this),
      httpChecker(state, *this),
      indexProofs(state, *this),
      intlChecker(state, *this),
      jsonChecker(state, *this),
      jumpChecker(state, *this),
      lateClasses(state, *this),
      loopChecker(state, *this),
      loopScanner(state, *this),
      mathChecker(state, *this),
      nameChecker(state, *this),
      pathChecker(state, *this),
      resultTypes(state, *this),
      stmtChecker(state, *this),
      arrayMethods(state, *this),
      asyncChecker(state, *this),
      bytesChecker(state, *this),
      childChecker(state, *this),
      classChecker(state, *this),
      cryptoErrors(state, *this),
      importBinder(state, *this),
      libraryCalls(state, *this),
      literalTypes(state, *this),
      matchChecker(state, *this),
      regexChecker(state, *this),
      timerChecker(state, *this),
      truthChecker(state, *this),
      tupleChecker(state, *this),
      typeDeclarer(state, *this),
      typeResolver(state, *this),
      unaryChecker(state, *this),
      unionChecker(state, *this),
      utilityTypes(state, *this),
      assignChecker(state, *this),
      binaryChecker(state, *this),
      bufferChecker(state, *this),
      classDeclarer(state, *this),
      concatChecker(state, *this),
      cryptoChecker(state, *this),
      memberChecker(state, *this),
      numberMethods(state, *this),
      objectChecker(state, *this),
      resultChecker(state, *this),
      returnChecker(state, *this),
      staticChecker(state, *this),
      streamChecker(state, *this),
      streamSources(state, *this),
      stringMethods(state, *this),
      switchChecker(state, *this),
      targetChecker(state, *this),
      templateTypes(state, *this),
      typeOperators(state, *this),
      typeRelations(state, *this),
      argListChecker(state, *this),
      assignedFields(state, *this),
      closureChecker(state, *this),
      conditionFacts(state, *this),
      consoleChecker(state, *this),
      constantFolder(state, *this),
      decimalChecker(state, *this),
      dictNewChecker(state, *this),
      emitterChecker(state, *this),
      genericChecker(state, *this),
      inheritChecker(state, *this),
      keyPairChecker(state, *this),
      literalChecker(state, *this),
      moduleDeclarer(state, *this),
      processChecker(state, *this),
      promiseChecker(state, *this),
      reflectChecker(state, *this),
      returnInferrer(state, *this),
      streamPipeline(state, *this),
      unionNarrowing(state, *this),
      unknownChecker(state, *this),
      weakRefChecker(state, *this),
      argumentChecker(state, *this),
      bytesNewChecker(state, *this),
      callbackChecker(state, *this),
      childErrorTypes(state, *this),
      extendsRelation(state, *this),
      functionChecker(state, *this),
      genericWidening(state, *this),
      iterableChecker(state, *this),
      iteratorMethods(state, *this),
      metadataChecker(state, *this),
      metadataWriters(state),
      nestedFunctions(state, *this),
      readlineChecker(state, *this),
      returnCompleter(state, *this),
      topLevelChecker(state, *this),
      typeArgInferrer(state, *this),
      conditionalTypes(state, *this),
      constructorCalls(state),
      decoratorChecker(state, *this),
      generatorChecker(state, *this),
      namesInFunctions(state, *this),
      overloadResolver(state, *this),
      predicateChecker(state, *this),
      streamNewChecker(state, *this),
      tzModulesChecker(state, *this),
      webCryptoChecker(state, *this),
      aliasInstantiator(state, *this),
      classValueChecker(state, *this),
      dateFormatChecker(state, *this),
      eventMapInference(state, *this),
      fsPromisesChecker(state, *this),
      instanceOfChecker(state, *this),
      intlMethodChecker(state, *this),
      jsonStaticChecker(state, *this),
      keyedFieldChecker(state, *this),
      methodCallChecker(state, *this),
      subtleCallChecker(state, *this),
      typedArrayChecker(state, *this),
      wideningRelations(state),
      builtinModuleTypes(state, *this),
      classMemberChecker(state, *this),
      declarationChecker(state, *this),
      destinationScanner(state, *this),
      eventTargetChecker(state, *this),
      intlOptionsChecker(state, *this),
      reflectCallChecker(state, *this),
      setEncodingChecker(state, *this),
      stringRegexChecker(state, *this),
      arrayLiteralChecker(state, *this),
      bufferStaticChecker(state, *this),
      cryptoMethodChecker(state, *this),
      genericClassChecker(state, *this),
      genericInstantiator(state, *this),
      resultMethodChecker(state, *this),
      builtinModuleChecker(state, *this),
      functionValueChecker(state, *this),
      methodLiteralChecker(state, *this),
      objectLiteralChecker(state, *this),
      promiseStaticChecker(state, *this),
      recordLiteralChecker(state, *this),
      runtimeObjectChecker(state, *this),
      cryptoFunctionChecker(state, *this),
      functionMethodChecker(state, *this),
      globalFunctionChecker(state, *this),
      genericClassInstantiator(state, *this) {}

Scopes &SemanticParts::getScopes() {
  return this->scopes;
}

Prelude &SemanticParts::getPrelude() {
  return this->prelude;
}

FsErrors &SemanticParts::getFsErrors() {
  return this->fsErrors;
}

EventMaps &SemanticParts::getEventMaps() {
  return this->eventMaps;
}

FsChecker &SemanticParts::getFsChecker() {
  return this->fsChecker;
}

OsChecker &SemanticParts::getOsChecker() {
  return this->osChecker;
}

ByteProofs &SemanticParts::getByteProofs() {
  return this->byteProofs;
}

CallCycles &SemanticParts::getCallCycles() {
  return this->callCycles;
}

ChunkTypes &SemanticParts::getChunkTypes() {
  return this->chunkTypes;
}

ClassBases &SemanticParts::getClassBases() {
  return this->classBases;
}

ClassLinks &SemanticParts::getClassLinks() {
  return this->classLinks;
}

CryptoArgs &SemanticParts::getCryptoArgs() {
  return this->cryptoArgs;
}

FieldFacts &SemanticParts::getFieldFacts() {
  return this->fieldFacts;
}

FsDeclarer &SemanticParts::getFsDeclarer() {
  return this->fsDeclarer;
}

LoopBounds &SemanticParts::getLoopBounds() {
  return this->loopBounds;
}

NewChecker &SemanticParts::getNewChecker() {
  return this->newChecker;
}

TypeParams &SemanticParts::getTypeParams() {
  return this->typeParams;
}

UrlChecker &SemanticParts::getUrlChecker() {
  return this->urlChecker;
}

VarChecker &SemanticParts::getVarChecker() {
  return this->varChecker;
}

CallChecker &SemanticParts::getCallChecker() {
  return this->callChecker;
}

DateChecker &SemanticParts::getDateChecker() {
  return this->dateChecker;
}

Destination &SemanticParts::getDestination() {
  return this->destination;
}

DictChecker &SemanticParts::getDictChecker() {
  return this->dictChecker;
}

DictMethods &SemanticParts::getDictMethods() {
  return this->dictMethods;
}

ExprChecker &SemanticParts::getExprChecker() {
  return this->exprChecker;
}

FlowChecker &SemanticParts::getFlowChecker() {
  return this->flowChecker;
}

HttpChecker &SemanticParts::getHttpChecker() {
  return this->httpChecker;
}

IndexProofs &SemanticParts::getIndexProofs() {
  return this->indexProofs;
}

IntlChecker &SemanticParts::getIntlChecker() {
  return this->intlChecker;
}

JsonChecker &SemanticParts::getJsonChecker() {
  return this->jsonChecker;
}

JumpChecker &SemanticParts::getJumpChecker() {
  return this->jumpChecker;
}

LateClasses &SemanticParts::getLateClasses() {
  return this->lateClasses;
}

LoopChecker &SemanticParts::getLoopChecker() {
  return this->loopChecker;
}

LoopScanner &SemanticParts::getLoopScanner() {
  return this->loopScanner;
}

MathChecker &SemanticParts::getMathChecker() {
  return this->mathChecker;
}

NameChecker &SemanticParts::getNameChecker() {
  return this->nameChecker;
}

PathChecker &SemanticParts::getPathChecker() {
  return this->pathChecker;
}

ResultTypes &SemanticParts::getResultTypes() {
  return this->resultTypes;
}

StmtChecker &SemanticParts::getStmtChecker() {
  return this->stmtChecker;
}

ArrayMethods &SemanticParts::getArrayMethods() {
  return this->arrayMethods;
}

AsyncChecker &SemanticParts::getAsyncChecker() {
  return this->asyncChecker;
}

BytesChecker &SemanticParts::getBytesChecker() {
  return this->bytesChecker;
}

ChildChecker &SemanticParts::getChildChecker() {
  return this->childChecker;
}

ClassChecker &SemanticParts::getClassChecker() {
  return this->classChecker;
}

CryptoErrors &SemanticParts::getCryptoErrors() {
  return this->cryptoErrors;
}

ImportBinder &SemanticParts::getImportBinder() {
  return this->importBinder;
}

LibraryCalls &SemanticParts::getLibraryCalls() {
  return this->libraryCalls;
}

LiteralTypes &SemanticParts::getLiteralTypes() {
  return this->literalTypes;
}

MatchChecker &SemanticParts::getMatchChecker() {
  return this->matchChecker;
}

RegexChecker &SemanticParts::getRegexChecker() {
  return this->regexChecker;
}

TimerChecker &SemanticParts::getTimerChecker() {
  return this->timerChecker;
}

TruthChecker &SemanticParts::getTruthChecker() {
  return this->truthChecker;
}

TupleChecker &SemanticParts::getTupleChecker() {
  return this->tupleChecker;
}

TypeDeclarer &SemanticParts::getTypeDeclarer() {
  return this->typeDeclarer;
}

TypeResolver &SemanticParts::getTypeResolver() {
  return this->typeResolver;
}

UnaryChecker &SemanticParts::getUnaryChecker() {
  return this->unaryChecker;
}

UnionChecker &SemanticParts::getUnionChecker() {
  return this->unionChecker;
}

UtilityTypes &SemanticParts::getUtilityTypes() {
  return this->utilityTypes;
}

AssignChecker &SemanticParts::getAssignChecker() {
  return this->assignChecker;
}

BinaryChecker &SemanticParts::getBinaryChecker() {
  return this->binaryChecker;
}

BufferChecker &SemanticParts::getBufferChecker() {
  return this->bufferChecker;
}

ClassDeclarer &SemanticParts::getClassDeclarer() {
  return this->classDeclarer;
}

ConcatChecker &SemanticParts::getConcatChecker() {
  return this->concatChecker;
}

CryptoChecker &SemanticParts::getCryptoChecker() {
  return this->cryptoChecker;
}

MemberChecker &SemanticParts::getMemberChecker() {
  return this->memberChecker;
}

NumberMethods &SemanticParts::getNumberMethods() {
  return this->numberMethods;
}

ObjectChecker &SemanticParts::getObjectChecker() {
  return this->objectChecker;
}

ResultChecker &SemanticParts::getResultChecker() {
  return this->resultChecker;
}

ReturnChecker &SemanticParts::getReturnChecker() {
  return this->returnChecker;
}

StaticChecker &SemanticParts::getStaticChecker() {
  return this->staticChecker;
}

StreamChecker &SemanticParts::getStreamChecker() {
  return this->streamChecker;
}

StreamSources &SemanticParts::getStreamSources() {
  return this->streamSources;
}

StringMethods &SemanticParts::getStringMethods() {
  return this->stringMethods;
}

SwitchChecker &SemanticParts::getSwitchChecker() {
  return this->switchChecker;
}

TargetChecker &SemanticParts::getTargetChecker() {
  return this->targetChecker;
}

TemplateTypes &SemanticParts::getTemplateTypes() {
  return this->templateTypes;
}

TypeOperators &SemanticParts::getTypeOperators() {
  return this->typeOperators;
}

TypeRelations &SemanticParts::getTypeRelations() {
  return this->typeRelations;
}

ArgListChecker &SemanticParts::getArgListChecker() {
  return this->argListChecker;
}

AssignedFields &SemanticParts::getAssignedFields() {
  return this->assignedFields;
}

ClosureChecker &SemanticParts::getClosureChecker() {
  return this->closureChecker;
}

ConditionFacts &SemanticParts::getConditionFacts() {
  return this->conditionFacts;
}

ConsoleChecker &SemanticParts::getConsoleChecker() {
  return this->consoleChecker;
}

ConstantFolder &SemanticParts::getConstantFolder() {
  return this->constantFolder;
}

DecimalChecker &SemanticParts::getDecimalChecker() {
  return this->decimalChecker;
}

DictNewChecker &SemanticParts::getDictNewChecker() {
  return this->dictNewChecker;
}

EmitterChecker &SemanticParts::getEmitterChecker() {
  return this->emitterChecker;
}

GenericChecker &SemanticParts::getGenericChecker() {
  return this->genericChecker;
}

InheritChecker &SemanticParts::getInheritChecker() {
  return this->inheritChecker;
}

KeyPairChecker &SemanticParts::getKeyPairChecker() {
  return this->keyPairChecker;
}

LiteralChecker &SemanticParts::getLiteralChecker() {
  return this->literalChecker;
}

ModuleDeclarer &SemanticParts::getModuleDeclarer() {
  return this->moduleDeclarer;
}

ProcessChecker &SemanticParts::getProcessChecker() {
  return this->processChecker;
}

PromiseChecker &SemanticParts::getPromiseChecker() {
  return this->promiseChecker;
}

ReflectChecker &SemanticParts::getReflectChecker() {
  return this->reflectChecker;
}

ReturnInferrer &SemanticParts::getReturnInferrer() {
  return this->returnInferrer;
}

StreamPipeline &SemanticParts::getStreamPipeline() {
  return this->streamPipeline;
}

UnionNarrowing &SemanticParts::getUnionNarrowing() {
  return this->unionNarrowing;
}

UnknownChecker &SemanticParts::getUnknownChecker() {
  return this->unknownChecker;
}

WeakRefChecker &SemanticParts::getWeakRefChecker() {
  return this->weakRefChecker;
}

ArgumentChecker &SemanticParts::getArgumentChecker() {
  return this->argumentChecker;
}

BytesNewChecker &SemanticParts::getBytesNewChecker() {
  return this->bytesNewChecker;
}

CallbackChecker &SemanticParts::getCallbackChecker() {
  return this->callbackChecker;
}

ChildErrorTypes &SemanticParts::getChildErrorTypes() {
  return this->childErrorTypes;
}

ExtendsRelation &SemanticParts::getExtendsRelation() {
  return this->extendsRelation;
}

FunctionChecker &SemanticParts::getFunctionChecker() {
  return this->functionChecker;
}

GenericWidening &SemanticParts::getGenericWidening() {
  return this->genericWidening;
}

IterableChecker &SemanticParts::getIterableChecker() {
  return this->iterableChecker;
}

IteratorMethods &SemanticParts::getIteratorMethods() {
  return this->iteratorMethods;
}

MetadataChecker &SemanticParts::getMetadataChecker() {
  return this->metadataChecker;
}

MetadataWriters &SemanticParts::getMetadataWriters() {
  return this->metadataWriters;
}

NestedFunctions &SemanticParts::getNestedFunctions() {
  return this->nestedFunctions;
}

ReadlineChecker &SemanticParts::getReadlineChecker() {
  return this->readlineChecker;
}

ReturnCompleter &SemanticParts::getReturnCompleter() {
  return this->returnCompleter;
}

TopLevelChecker &SemanticParts::getTopLevelChecker() {
  return this->topLevelChecker;
}

TypeArgInferrer &SemanticParts::getTypeArgInferrer() {
  return this->typeArgInferrer;
}

ConditionalTypes &SemanticParts::getConditionalTypes() {
  return this->conditionalTypes;
}

ConstructorCalls &SemanticParts::getConstructorCalls() {
  return this->constructorCalls;
}

DecoratorChecker &SemanticParts::getDecoratorChecker() {
  return this->decoratorChecker;
}

GeneratorChecker &SemanticParts::getGeneratorChecker() {
  return this->generatorChecker;
}

NamesInFunctions &SemanticParts::getNamesInFunctions() {
  return this->namesInFunctions;
}

OverloadResolver &SemanticParts::getOverloadResolver() {
  return this->overloadResolver;
}

PredicateChecker &SemanticParts::getPredicateChecker() {
  return this->predicateChecker;
}

StreamNewChecker &SemanticParts::getStreamNewChecker() {
  return this->streamNewChecker;
}

TzModulesChecker &SemanticParts::getTzModulesChecker() {
  return this->tzModulesChecker;
}

WebCryptoChecker &SemanticParts::getWebCryptoChecker() {
  return this->webCryptoChecker;
}

AliasInstantiator &SemanticParts::getAliasInstantiator() {
  return this->aliasInstantiator;
}

ClassValueChecker &SemanticParts::getClassValueChecker() {
  return this->classValueChecker;
}

DateFormatChecker &SemanticParts::getDateFormatChecker() {
  return this->dateFormatChecker;
}

EventMapInference &SemanticParts::getEventMapInference() {
  return this->eventMapInference;
}

FsPromisesChecker &SemanticParts::getFsPromisesChecker() {
  return this->fsPromisesChecker;
}

InstanceOfChecker &SemanticParts::getInstanceOfChecker() {
  return this->instanceOfChecker;
}

IntlMethodChecker &SemanticParts::getIntlMethodChecker() {
  return this->intlMethodChecker;
}

JsonStaticChecker &SemanticParts::getJsonStaticChecker() {
  return this->jsonStaticChecker;
}

KeyedFieldChecker &SemanticParts::getKeyedFieldChecker() {
  return this->keyedFieldChecker;
}

MethodCallChecker &SemanticParts::getMethodCallChecker() {
  return this->methodCallChecker;
}

SubtleCallChecker &SemanticParts::getSubtleCallChecker() {
  return this->subtleCallChecker;
}

TypedArrayChecker &SemanticParts::getTypedArrayChecker() {
  return this->typedArrayChecker;
}

WideningRelations &SemanticParts::getWideningRelations() {
  return this->wideningRelations;
}

BuiltinModuleTypes &SemanticParts::getBuiltinModuleTypes() {
  return this->builtinModuleTypes;
}

ClassMemberChecker &SemanticParts::getClassMemberChecker() {
  return this->classMemberChecker;
}

DeclarationChecker &SemanticParts::getDeclarationChecker() {
  return this->declarationChecker;
}

DestinationScanner &SemanticParts::getDestinationScanner() {
  return this->destinationScanner;
}

EventTargetChecker &SemanticParts::getEventTargetChecker() {
  return this->eventTargetChecker;
}

IntlOptionsChecker &SemanticParts::getIntlOptionsChecker() {
  return this->intlOptionsChecker;
}

ReflectCallChecker &SemanticParts::getReflectCallChecker() {
  return this->reflectCallChecker;
}

SetEncodingChecker &SemanticParts::getSetEncodingChecker() {
  return this->setEncodingChecker;
}

StringRegexChecker &SemanticParts::getStringRegexChecker() {
  return this->stringRegexChecker;
}

ArrayLiteralChecker &SemanticParts::getArrayLiteralChecker() {
  return this->arrayLiteralChecker;
}

BufferStaticChecker &SemanticParts::getBufferStaticChecker() {
  return this->bufferStaticChecker;
}

CryptoMethodChecker &SemanticParts::getCryptoMethodChecker() {
  return this->cryptoMethodChecker;
}

GenericClassChecker &SemanticParts::getGenericClassChecker() {
  return this->genericClassChecker;
}

GenericInstantiator &SemanticParts::getGenericInstantiator() {
  return this->genericInstantiator;
}

ResultMethodChecker &SemanticParts::getResultMethodChecker() {
  return this->resultMethodChecker;
}

BuiltinModuleChecker &SemanticParts::getBuiltinModuleChecker() {
  return this->builtinModuleChecker;
}

FunctionValueChecker &SemanticParts::getFunctionValueChecker() {
  return this->functionValueChecker;
}

MethodLiteralChecker &SemanticParts::getMethodLiteralChecker() {
  return this->methodLiteralChecker;
}

ObjectLiteralChecker &SemanticParts::getObjectLiteralChecker() {
  return this->objectLiteralChecker;
}

PromiseStaticChecker &SemanticParts::getPromiseStaticChecker() {
  return this->promiseStaticChecker;
}

RecordLiteralChecker &SemanticParts::getRecordLiteralChecker() {
  return this->recordLiteralChecker;
}

RuntimeObjectChecker &SemanticParts::getRuntimeObjectChecker() {
  return this->runtimeObjectChecker;
}

CryptoFunctionChecker &SemanticParts::getCryptoFunctionChecker() {
  return this->cryptoFunctionChecker;
}

FunctionMethodChecker &SemanticParts::getFunctionMethodChecker() {
  return this->functionMethodChecker;
}

GlobalFunctionChecker &SemanticParts::getGlobalFunctionChecker() {
  return this->globalFunctionChecker;
}

GenericClassInstantiator &SemanticParts::getGenericClassInstantiator() {
  return this->genericClassInstantiator;
}

}
