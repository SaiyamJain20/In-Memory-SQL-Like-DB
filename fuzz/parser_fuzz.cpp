// libFuzzer harness for the SQL parser. Same contract as tests/parser/parser_fuzz_test.cpp:
//   * parse succeeds -> printing then re-parsing must reproduce the same text;
//   * otherwise Error(Syntax | NotImplemented) with an in-range position.
// Any violation aborts, which libFuzzer reports as a crash with the offending input.

#include "parser/parser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Violation(const char* what, const std::string& sql) {
    std::fprintf(stderr, "CONTRACT VIOLATION: %s\ninput (%zu bytes): %s\n", what, sql.size(),
                 sql.c_str());
    std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string sql(reinterpret_cast<const char*>(data), size);
    try {
        auto statements = cdb::ParseStatements(sql);
        std::string printed;
        for (const auto& s : statements) printed += s->ToString() + "; ";
        std::string again;
        try {
            for (const auto& s : cdb::ParseStatements(printed)) again += s->ToString() + "; ";
        } catch (const cdb::Error&) {
            Violation("printed SQL does not re-parse", sql);
        }
        if (printed != again) Violation("print/parse is not a fixpoint", sql);
    } catch (const cdb::Error& e) {
        if (e.code() != cdb::ErrorCode::Syntax && e.code() != cdb::ErrorCode::NotImplemented) {
            Violation("unexpected error code", sql);
        }
        if (e.position().has_value() && *e.position() > sql.size()) {
            Violation("error position beyond the input", sql);
        }
    }
    return 0;
}
