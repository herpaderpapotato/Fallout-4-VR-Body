// FRIK-flat C16: crash diagnostics for bring-up (this install has no crash logger). A vectored handler logs the first
// few access violations / illegal instructions / stack-buffer overruns: code, faulting address as module+offset, and an
// unwound call stack (RtlVirtualUnwind), symbolised with DbgHelp against FRIK.pdb next to FRIK.dll. It only logs and
// returns EXCEPTION_CONTINUE_SEARCH, so the process behaves exactly as without it.
#include "vrshim/FlatCompat.h"

#include <DbgHelp.h>
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace vrshim
{
    namespace
    {
        std::atomic<int> g_logged{ 0 };

        std::string Describe(const DWORD64 a_addr)
        {
            HMODULE mod = nullptr;
            char path[MAX_PATH]{};
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_addr), &mod) && mod) {
                ::GetModuleFileNameA(mod, path, MAX_PATH);
            }
            const char* name = path[0] ? std::strrchr(path, '\\') : nullptr;
            name = name ? name + 1 : (path[0] ? path : "?");
            std::string out = std::format("{}+0x{:X}", name, mod ? a_addr - reinterpret_cast<DWORD64>(mod) : a_addr);
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (::SymFromAddr(::GetCurrentProcess(), a_addr, &disp, sym)) {
                out += std::format(" {}+0x{:X}", sym->Name, disp);
                IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
                DWORD ld = 0;
                if (::SymGetLineFromAddr64(::GetCurrentProcess(), a_addr, &ld, &line)) {
                    const char* f = std::strrchr(line.FileName, '\\');
                    out += std::format(" ({}:{})", f ? f + 1 : line.FileName, line.LineNumber);
                }
            }
            return out;
        }

        LONG CALLBACK Handler(EXCEPTION_POINTERS* a_ep)
        {
            const DWORD code = a_ep->ExceptionRecord->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != STATUS_STACK_BUFFER_OVERRUN && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
                code != EXCEPTION_PRIV_INSTRUCTION) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (g_logged.fetch_add(1) >= 3) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            static bool symInit = false;
            if (!symInit) {
                symInit = true;
                ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
            }
            const auto* rec = a_ep->ExceptionRecord;
            std::string info;
            if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
                info = std::format(" ({} 0x{:X})", rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "exec", rec->ExceptionInformation[1]);
            }
            spdlog::critical("FRIK-flat CRASH(first chance) code 0x{:08X}{} at {} thread {}",
                code,
                info,
                Describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress)),
                ::GetCurrentThreadId());
            CONTEXT ctx = *a_ep->ContextRecord;
            for (int i = 0; i < 24 && ctx.Rip; ++i) {
                spdlog::critical("  #{:02} {}", i, Describe(ctx.Rip));
                DWORD64 base = 0;
                auto* fe = ::RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fe) { // leaf function: return address at [rsp]
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr;
                DWORD64 frame = 0;
                ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &handlerData, &frame, nullptr);
            }
            spdlog::default_logger()->flush();
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void installCrashLog()
    {
        static bool done = false;
        if (!done) {
            done = true;
            ::AddVectoredExceptionHandler(1, &Handler);
        }
    }
}
