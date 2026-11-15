#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace minifs {

// Splits a command line into words. Text in "double" or 'single' quotes is
// one word; inside double quotes \n, \t, \" and \\ are escapes.
// Throws std::invalid_argument on an unterminated quote.
std::vector<std::string> tokenize(const std::string& line);

class Cli {
public:
    struct Options {
        std::string disk_path = "virtual_disk.bin";
        // Print each command after the prompt (for scripts and piped input).
        bool echo = false;
        // For programs driving MiniFS (the web GUI): no banner, prompt or
        // echo; command output on stdout exactly as is, errors on stderr.
        bool quiet = false;
    };

    Cli();
    explicit Cli(Options options);

    // Runs commands until 'exit' or end of input. Returns the process exit
    // code: 0 normally, 3 after a simulated crash.
    int run(std::istream& input, std::ostream& output);

private:
    Options options_;
};

} // namespace minifs
