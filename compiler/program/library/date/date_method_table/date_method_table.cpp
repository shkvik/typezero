#include "compiler/program/library/date/date_method_table/date_method_table.h"
#include "runtime/library/date/date/date.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const DateMethodInfo kDateMethods[] = {
    {
        .name = "getTime",
        .builtin = Builtin::DateGetTime,
        .form = DateMethodForm::GET_TIME,
        .field = 0,
        .min = 0,
        .max = 0,
    },
    {
        .name = "valueOf",
        .builtin = Builtin::DateGetTime,
        .form = DateMethodForm::GET_TIME,
        .field = 0,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getFullYear",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_YEAR,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getMonth",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MONTH,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getDate",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_DATE,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getDay",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_DAY,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getHours",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_HOURS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getMinutes",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MINUTES,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getSeconds",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_SECONDS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getMilliseconds",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getTimezoneOffset",
        .builtin = Builtin::DateGetLocal,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_OFFSET,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCFullYear",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_YEAR,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCMonth",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MONTH,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCDate",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_DATE,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCDay",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_DAY,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCHours",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_HOURS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCMinutes",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MINUTES,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCSeconds",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_SECONDS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "getUTCMilliseconds",
        .builtin = Builtin::DateGetUTC,
        .form = DateMethodForm::GET_FIELD,
        .field = TZ_DATE_MS,
        .min = 0,
        .max = 0,
    },
    {
        .name = "setTime",
        .builtin = Builtin::DateSetTime,
        .form = DateMethodForm::SET_TIME,
        .field = 0,
        .min = 1,
        .max = 1,
    },
    {
        .name = "setMilliseconds",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MS,
        .min = 1,
        .max = 1,
    },
    {
        .name = "setSeconds",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_SECONDS,
        .min = 1,
        .max = 2,
    },
    {
        .name = "setMinutes",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MINUTES,
        .min = 1,
        .max = 3,
    },
    {
        .name = "setHours",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_HOURS,
        .min = 1,
        .max = 4,
    },
    {
        .name = "setDate",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_DATE,
        .min = 1,
        .max = 1,
    },
    {
        .name = "setMonth",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MONTH,
        .min = 1,
        .max = 2,
    },
    {
        .name = "setFullYear",
        .builtin = Builtin::DateSetLocal,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_YEAR,
        .min = 1,
        .max = 3,
    },
    {
        .name = "setUTCMilliseconds",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MS,
        .min = 1,
        .max = 1,
    },
    {
        .name = "setUTCSeconds",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_SECONDS,
        .min = 1,
        .max = 2,
    },
    {
        .name = "setUTCMinutes",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MINUTES,
        .min = 1,
        .max = 3,
    },
    {
        .name = "setUTCHours",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_HOURS,
        .min = 1,
        .max = 4,
    },
    {
        .name = "setUTCDate",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_DATE,
        .min = 1,
        .max = 1,
    },
    {
        .name = "setUTCMonth",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_MONTH,
        .min = 1,
        .max = 2,
    },
    {
        .name = "setUTCFullYear",
        .builtin = Builtin::DateSetUTC,
        .form = DateMethodForm::SET_FIELDS,
        .field = TZ_DATE_YEAR,
        .min = 1,
        .max = 3,
    },
    {
        .name = "toISOString",
        .builtin = Builtin::DateToISOString,
        .form = DateMethodForm::TO_TEXT,
        .field = 0,
        .min = 0,
        .max = 0,
    },
    {
        .name = "toJSON",
        .builtin = Builtin::DateToJSON,
        .form = DateMethodForm::TO_JSON,
        .field = 0,
        .min = 0,
        .max = 1,
    },
    {
        .name = "toUTCString",
        .builtin = Builtin::DateToUTCString,
        .form = DateMethodForm::TO_TEXT,
        .field = 0,
        .min = 0,
        .max = 0,
    },
    {
        .name = "toString",
        .builtin = Builtin::DateToLocalString,
        .form = DateMethodForm::TO_FIELD_TEXT,
        .field = TZ_DATE_TEXT_FULL,
        .min = 0,
        .max = 0,
    },
    {
        .name = "toDateString",
        .builtin = Builtin::DateToLocalString,
        .form = DateMethodForm::TO_FIELD_TEXT,
        .field = TZ_DATE_TEXT_DATE,
        .min = 0,
        .max = 0,
    },
    {
        .name = "toTimeString",
        .builtin = Builtin::DateToLocalString,
        .form = DateMethodForm::TO_FIELD_TEXT,
        .field = TZ_DATE_TEXT_TIME,
        .min = 0,
        .max = 0,
    },
    {
        .name = "toLocaleString",
        .builtin = Builtin::DateToLocale,
        .form = DateMethodForm::TO_LOCALE,
        .field = TZ_DATE_LOCALE_ALL,
        .min = 0,
        .max = 2,
    },
    {
        .name = "toLocaleDateString",
        .builtin = Builtin::DateToLocale,
        .form = DateMethodForm::TO_LOCALE,
        .field = TZ_DATE_LOCALE_DATE,
        .min = 0,
        .max = 2,
    },
    {
        .name = "toLocaleTimeString",
        .builtin = Builtin::DateToLocale,
        .form = DateMethodForm::TO_LOCALE,
        .field = TZ_DATE_LOCALE_TIME,
        .min = 0,
        .max = 2,
    },
};

}

const DateMethodInfo *DateMethodTable::find(llvm::StringRef name) {
  for (const DateMethodInfo &info : kDateMethods) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

llvm::StringRef DateMethodTable::getName(Builtin builtin, int field) {
  for (const DateMethodInfo &info : kDateMethods) {
    if (info.builtin == builtin && info.field == field) {
      return info.name;
    }
  }
  llvm_unreachable("date method missing from the table");
}

}
