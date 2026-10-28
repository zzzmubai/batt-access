#pragma once
#include <stdexcept>
#include <string>

namespace batt {
// Messages contain operation names/codes, never raw connector errors or SQL values.
struct StorageError : std::runtime_error {
    unsigned code = 0;
    bool commit_unknown = false;
    explicit StorageError(const std::string& operation, unsigned error_code = 0, bool unknown = false)
        : std::runtime_error(operation + (error_code ? " mysql_code=" + std::to_string(error_code) : "") +
                             (unknown ? " commit_outcome=unknown; transaction_not_replayed" : "")),
          code(error_code), commit_unknown(unknown) {}
};
} // namespace batt
