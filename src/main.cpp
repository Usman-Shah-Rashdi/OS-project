// MiniFS entry point.
//
//   minifs                     interactive shell on virtual_disk.bin
//   minifs script.txt          run the commands in script.txt
//   minifs --disk other.bin    use a different disk image

#include "minifs/cli.hpp"

#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define MINIFS_ISATTY _isatty
#define MINIFS_FILENO _fileno
#else
#include <unistd.h>
#define MINIFS_ISATTY isatty
#define MINIFS_FILENO fileno
#endif

using namespace std;

int main(int argc, char** argv) {
    minifs::Cli::Options options;
    string script;
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--disk" && i + 1 < argc) {
            options.disk_path = argv[++i];
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            cout << "Usage: minifs [--disk FILE] [--quiet] [SCRIPT]\n"
                 << "  --disk FILE  disk image to use (default: virtual_disk.bin)\n"
                 << "  --quiet      no banner or prompt; errors go to stderr (for programs)\n"
                 << "  SCRIPT       text file with one command per line\n";
            return 0;
        } else if (script.empty() && arg.rfind("--", 0) != 0) {
            script = arg;
        } else {
            cerr << "Unknown argument: " << arg << " (try --help)\n";
            return 1;
        }
    }

#ifdef _WIN32
    if (options.quiet) {
        // Programs reading our output need the exact bytes of a file, so do
        // not let Windows turn "\n" into "\r\n".
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif

    if (!script.empty()) {
        ifstream file(script);
        if (!file) {
            cerr << "Cannot open script: " << script << '\n';
            return 1;
        }
        options.echo = true;
        return minifs::Cli(options).run(file, cout);
    }

    // When input is piped or redirected, show each command after the prompt
    // so the output reads like a terminal session.
    options.echo = !MINIFS_ISATTY(MINIFS_FILENO(stdin));
    return minifs::Cli(options).run(cin, cout);
}
