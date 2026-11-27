-- FRIK-flat: FRIK (Fallout 4 VR Body) + the parts of F4VR-CommonFramework it uses, built against the flat
-- CommonLibF4 (extern/commonlibf4, Fallout 4 1.11.240) for the fo4flatvr feasibility build.
-- Design and change log: analysis/frik-flat-port.md. Sources are copies of mods/Fallout-4-VR-Body (3d79219e, synced 27 Nov 2026)
-- and its F4VR-CommonFramework submodule (14a76b94; game-RE layer kept from 5677388, see C46); mods/ is never edited.
--
-- Separate xmake project (own .xmake/ and build/), so it never touches the plugin's build:
--   cd frik-flat && python -I tools/gen.py && xmake f -m releasedbg -y && xmake build FRIK
set_xmakever("3.0.0")
set_project("frik-flat")
set_version("0.80.3")
set_allowedplats("windows")
set_defaultplat("windows")
set_arch("x64")
set_languages("c++23")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")

includes("external/commonlibf4")

local common_defines = {
    "_UNICODE", "UNICODE", "NOMMNOSOUND", "_CRT_SECURE_NO_WARNINGS",
    "TOML_HEADER_ONLY=0",               -- toml++ compiled once in fw-src/common/TomlImpl.cpp
    "VR_API_EXPORT",                    -- openvr.h without dllimport: vrshim/OpenVRStub.cpp defines the entry points
                                        -- (this openvr.h has no static mode on Win32; FRIK.dll exports them, harmless)
    "F4VR_WITH_OPENVR=1",
    "FRIK_FLAT=1",                      -- the flat port's own #ifs (see analysis/frik-flat-port.md change log)
}

local function common(target_includes_frik)
    add_defines(common_defines)
    add_includedirs("vrshim/include", "third_party/include", "external/F4VR-CommonFramework/external/openvr", "third_party/stb", "generated")
    add_cxflags("/permissive-", "/Zc:preprocessor", "/Zc:__cplusplus", "/bigobj", "/EHsc", "/W3", { force = true })
    -- the flat CommonLibF4 headers and the copied sources are not warning-clean together; errors are what matter here
    add_cxflags("/wd4100", "/wd4189", "/wd4996", "/wd4244", "/wd4267", { force = true })
end

-- F4VR-CommonFramework (subset: everything but the Dear ImGui layer), as a static library.
target("f4cf", function()
    set_kind("static")
    set_default(false)
    add_deps("commonlibf4")
    common()
    add_includedirs("external/F4VR-CommonFramework/src", { public = true })
    add_files("external/F4VR-CommonFramework/src/**.cpp|imgui/**.cpp")
    add_files("generated/EmbeddedTextFont.cpp")
    add_files("vrshim/src/**.cpp")
    set_pcxxheader("external/F4VR-CommonFramework/src/PCH.h")
    add_defines('F4CF_VERSION="0.5.1"')
    add_syslinks("d3d11", "d3dcompiler", "dxgi", "user32", "advapi32", "shell32", "ole32", { public = true })
end)

-- FRIK.dll
target("FRIK", function()
    add_rules("commonlibf4.plugin", {
        name = "FRIK",
        author = "FRIK authors (flat port: local feasibility build)",
        description = "FRIK full body ported to flat Fallout 4 1.11.240 (fo4flatvr)"
    })
    add_deps("f4cf")
    common()
    add_includedirs("src")
    add_files("src/**.cpp")
    add_files("generated/resources.rc")
    set_pcxxheader("src/PCH.h")
end)
