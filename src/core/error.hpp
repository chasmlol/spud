#pragma once

#include <stdexcept>
#include <string>

namespace spud {

// Base for all spud exceptions. Throw for unrecoverable problems with a
// single file or operation; callers decide whether to skip or abort.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Malformed or truncated input data.
struct ParseError : Error {
    using Error::Error;
};

} // namespace spud
