-- Azeroth Theft Auto GTA SA adapter (Win32 .asi) - Plugin-SDK premake workflow.
--
-- Windows (MSVC, the SDK's primary workflow):
--   set PLUGIN_SDK_DIR=<abs path>\vendor\plugin-sdk   (Plugin-SDK built: output\lib\Plugin.lib)
--   <plugin-sdk>\tools\premake\premake5.exe --file=adapters\gta-sa\premake5.lua vs2022
--   msbuild build\gta-win32\AzerothTheftAuto.sln /p:Configuration=Release /p:Platform=Win32
-- Linux cross build (clang -> i686-w64-mingw32, SDK's linux-clang-example):
--   PLUGIN_SDK_DIR=... <plugin-sdk>/tools/premake/linux-clang-example/premake5 --file=adapters/gta-sa/premake5.lua gmake2
--   make -C build/gta-win32 config=release
local sdkdir = os.getenv("PLUGIN_SDK_DIR")
if not sdkdir or sdkdir == "" then
    error("PLUGIN_SDK_DIR is not set (absolute path to the pinned Plugin-SDK checkout)")
end
sdkdir = path.translate(sdkdir, "/")
local here = path.getabsolute(".")
local root = path.getabsolute("../..", here)
local mingw = _ACTION == "gmake" or _ACTION == "gmake2"

workspace "AzerothTheftAuto"
    location (root .. "/build/gta-win32")
    configurations { "Release", "Debug" }
    architecture "x86"

project "AzerothTheftAuto"
    kind "SharedLib"
    language "C++"
    targetextension ".asi"
    targetname "AzerothTheftAuto"
    targetprefix ""
    targetdir (root .. "/build/gta-win32/bin/%{cfg.buildcfg}")
    objdir (root .. "/build/gta-win32/obj/%{cfg.buildcfg}")
    characterset "MBCS"
    staticruntime "On"
    -- Plugin-SDK headers require its own dialect (its examples use C++latest / -std=c++2b).
    cppdialect "C++latest"

    files {
        here .. "/src/*.cpp",
        here .. "/src/*.h",
        root .. "/protocol/src/*.cpp",
        root .. "/protocol/include/gamebridge/*.h",
    }
    includedirs {
        here .. "/src",
        root .. "/protocol/include",
        sdkdir .. "/shared",
        sdkdir .. "/shared/game",
        sdkdir .. "/plugin_sa",
        sdkdir .. "/plugin_sa/game_sa",
        sdkdir .. "/plugin_sa/game_sa/enums",
        sdkdir .. "/plugin_sa/game_sa/rw",
    }
    defines { "GTASA", "PLUGIN_SGV_10US", "RW", "_CRT_SECURE_NO_WARNINGS", "_CRT_NON_CONFORMING_SWPRINTFS" }
    libdirs { sdkdir .. "/output/lib" }

    filter "configurations:Release"
        optimize "On"
        links { "Plugin", "ws2_32" }
    filter "configurations:Debug"
        symbols "On"
        defines "DEBUG"
        links { mingw and "Plugin" or "Plugin_d", "ws2_32" }
    filter {}

    if mingw then
        toolset "clang"
        buildoptions {
            "--target=i686-w64-mingw32", "-fpermissive", "-fcommon", "-fms-extensions",
            "-Wno-invalid-offsetof", "-Wno-microsoft-include", "-Wno-builtin-macro-redefined",
            "-D__cpp_concepts=202202L",
        }
        linkoptions { "--target=i686-w64-mingw32", "-static", "-static-libgcc", "-static-libstdc++" }
    else
        buildoptions { "/sdl-" }
        multiprocessorcompile "On"
    end
