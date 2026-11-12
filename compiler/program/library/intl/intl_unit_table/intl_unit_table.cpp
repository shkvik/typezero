#include "compiler/program/library/intl/intl_unit_table/intl_unit_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kPerSeparator = "-per-";

const llvm::StringLiteral kSanctionedUnits[] = {
    "acre",        "bit",         "byte",        "celsius",
    "centimeter",  "day",         "degree",      "fahrenheit",
    "fluid-ounce", "foot",        "gallon",      "gigabit",
    "gigabyte",    "gram",        "hectare",     "hour",
    "inch",        "kilobit",     "kilobyte",    "kilogram",
    "kilometer",   "liter",       "megabit",     "megabyte",
    "meter",       "microsecond", "mile",        "mile-scandinavian",
    "milliliter",  "millimeter",  "millisecond", "minute",
    "month",       "nanosecond",  "ounce",       "percent",
    "petabyte",    "pound",       "second",      "stone",
    "terabit",     "terabyte",    "week",        "yard",
    "year",
};

const llvm::StringLiteral kRelativeUnits[] = {
    "year", "years", "quarter", "quarters", "month",  "months",  "week",   "weeks",
    "day",  "days",  "hour",    "hours",    "minute", "minutes", "second", "seconds",
};

}

bool IntlUnitTable::isWellFormed(llvm::StringRef unit) {
  if (IntlUnitTable::isSanctioned(unit)) {
    return true;
  }
  auto [numerator, denominator] = unit.split(kPerSeparator);

  return !denominator.empty() && IntlUnitTable::isSanctioned(numerator) &&
         IntlUnitTable::isSanctioned(denominator);
}

llvm::ArrayRef<llvm::StringLiteral> IntlUnitTable::getRelativeUnits() {
  return kRelativeUnits;
}

bool IntlUnitTable::isSanctioned(llvm::StringRef unit) {
  return llvm::is_contained(kSanctionedUnits, unit);
}

}
