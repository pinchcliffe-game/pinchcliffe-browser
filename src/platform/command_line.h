#pragma once

#include <string>
#include <vector>

namespace fgp {

// The process arguments as UTF-8, in an argc/argv pair that lives as long as
// this object.
//
// Every other platform hands main() UTF-8 already. On Windows the bytes in
// argv depend on which entry point built them: SDL2main converts the wide
// command line to UTF-8, the CRT's own main uses the process code page. Both
// are replaced here by the wide command line itself, so a directory such as
// "Flåklypa Grand Prix" arrives intact whichever entry point linked.
class Utf8Arguments {
public:
    Utf8Arguments(int argc, char **argv);

    Utf8Arguments(const Utf8Arguments &) = delete;
    Utf8Arguments &operator=(const Utf8Arguments &) = delete;

    int argc() const { return static_cast<int>(storage_.size()); }
    char **argv() { return pointers_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<char *> pointers_;
};

} // namespace fgp
