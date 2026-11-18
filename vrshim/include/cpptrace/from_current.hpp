#pragma once
// FRIK-flat: minimal stand-in for cpptrace's from_current.hpp (FRIK/framework only use it to log a stack trace
// next to an exception). No stack trace in the flat build; the exception text is still logged.
#include <string>

#define CPPTRACE_TRY try
#define CPPTRACE_CATCH(...) catch (__VA_ARGS__)

namespace cpptrace
{
    struct stacktrace_stub
    {
        [[nodiscard]] std::string to_string() const
        {
            return "(no stack trace in the FRIK-flat build)";
        }
    };

    inline stacktrace_stub from_current_exception()
    {
        return {};
    }
}
