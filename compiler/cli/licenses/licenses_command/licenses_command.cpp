#include "compiler/cli/licenses/licenses_command/licenses_command.h"
#include "compiler/cli/command_line/args/args.h"
#include "compiler/link/licenses/licenses.h"

namespace tz {

LicensesCommand::LicensesCommand(const RuntimeFiles &runtime)
    : runtime(runtime) {}

int LicensesCommand::run(const Args &args) const {
  return Licenses::printLicenses(args.input, this->runtime.path("licenses"));
}

}
