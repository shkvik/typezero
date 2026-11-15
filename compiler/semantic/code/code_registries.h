#pragma once

#include "compiler/semantic/code/arithmetic/code_arithmetic_registry.h"
#include "compiler/semantic/code/assignments/code_assignments_registry.h"
#include "compiler/semantic/code/blocks/code_blocks_registry.h"
#include "compiler/semantic/code/branches/code_branches_registry.h"
#include "compiler/semantic/code/calls/code_calls_registry.h"
#include "compiler/semantic/code/classes/code_classes_registry.h"
#include "compiler/semantic/code/comparisons/code_comparisons_registry.h"
#include "compiler/semantic/code/concatenation/code_concatenation_registry.h"
#include "compiler/semantic/code/conversions/code_conversions_registry.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry.h"
#include "compiler/semantic/code/expressions/code_expressions_registry.h"
#include "compiler/semantic/code/functions/code_functions_registry.h"
#include "compiler/semantic/code/indexing/code_indexing_registry.h"
#include "compiler/semantic/code/iterators/code_iterators_registry.h"
#include "compiler/semantic/code/literals/code_literals_registry.h"
#include "compiler/semantic/code/logic/code_logic_registry.h"
#include "compiler/semantic/code/loops/code_loops_registry.h"
#include "compiler/semantic/code/members/code_members_registry.h"
#include "compiler/semantic/code/names/code_names_registry.h"
#include "compiler/semantic/code/objects/code_objects_registry.h"
#include "compiler/semantic/code/operators/code_operators_registry.h"
#include "compiler/semantic/code/returns/code_returns_registry.h"
#include "compiler/semantic/code/tuples/code_tuples_registry.h"
#include "compiler/semantic/code/unions/code_unions_registry.h"
#include "compiler/semantic/code/unknown_values/code_unknown_values_registry.h"
#include "compiler/semantic/code/variables/code_variables_registry.h"

namespace tz {

struct CodeRegistries {
  CodeCallsRegistry calls;
  CodeLogicRegistry logic;
  CodeLoopsRegistry loops;
  CodeNamesRegistry names;
  CodeBlocksRegistry blocks;
  CodeTuplesRegistry tuples;
  CodeUnionsRegistry unions;
  CodeClassesRegistry classes;
  CodeMembersRegistry members;
  CodeObjectsRegistry objects;
  CodeReturnsRegistry returns;
  CodeBranchesRegistry branches;
  CodeIndexingRegistry indexing;
  CodeLiteralsRegistry literals;
  CodeFunctionsRegistry functions;
  CodeIteratorsRegistry iterators;
  CodeOperatorsRegistry operators;
  CodeVariablesRegistry variables;
  CodeArithmeticRegistry arithmetic;
  CodeAssignmentsRegistry assignments;
  CodeComparisonsRegistry comparisons;
  CodeConversionsRegistry conversions;
  CodeExpressionsRegistry expressions;
  CodeDictionariesRegistry dictionaries;
  CodeConcatenationRegistry concatenation;
  CodeUnknownValuesRegistry unknownValues;
};

}
