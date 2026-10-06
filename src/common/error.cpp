#include "common/error.h"

namespace cdb {

const char* ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Internal:
        return "Internal Error";
    case ErrorCode::Syntax:
        return "Syntax Error";
    case ErrorCode::Binder:
        return "Binder Error";
    case ErrorCode::Catalog:
        return "Catalog Error";
    case ErrorCode::Type:
        return "Type Error";
    case ErrorCode::Execution:
        return "Execution Error";
    case ErrorCode::Io:
        return "IO Error";
    case ErrorCode::Corruption:
        return "Corruption Error";
    case ErrorCode::NotImplemented:
        return "Not Implemented";
    }
    return "Unknown Error";
}

Error::Error(ErrorCode code, const std::string& message)
    : std::runtime_error(std::string(ErrorCodeName(code)) + ": " + message), code_(code) {}

Error::Error(ErrorCode code, const std::string& message, size_t position)
    : std::runtime_error(std::string(ErrorCodeName(code)) + ": " + message), code_(code),
      position_(position) {}

} // namespace cdb
