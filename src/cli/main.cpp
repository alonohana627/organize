// `mso`: command-line access to Music Sample Organizer libraries (scripting, tests, headless use).
// Everything but the console set-up lives in the mso_cli library (command_line.cpp), so the CLI
// tests can run commands in-process.
#include "mso/cli/command_context.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // file names are UTF-8 (Hebrew, Japanese, ...)
#endif
    return mso::cli::run_main(argc, argv);
}
