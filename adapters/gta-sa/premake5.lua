-- Azeroth Theft Auto GTA SA adapter - Plugin-SDK premake workflow.
--
-- Two platforms from the same sources:
--   Classic : GTA SA 1.0 US, Win32  -> build/gta/bin/Classic/Release/AzerothTheftAuto.asi
--   DE      : GTA SA The Definitive Edition, x64 -> build/gta/bin/DE/Release/AzerothTheftAuto.asi
--
-- Windows (MSVC, the SDK's primary workflow):
--   set PLUGIN_SDK_DIR=<abs path>\vendor\plugin-sdk   (SDK libs built: output\lib\Plugin.lib, Plugin_Unreal.lib)
--   <plugin-sdk>\tools\premake\premake5.exe --file=adapters\gta-sa\premake5.lua vs2022
--   msbuild build\gta\AzerothTheftAuto.sln /p:Configuration=Release /p:Platform=DE
-- Linux cross build (clang -> *-w64-mingw32, SDK's linux-clang-example):
--   PLUGIN_SDK_DIR=... <plugin-sdk>/tools/premake/linux-clang-example/premake5 --file=adapters/gta-sa/premake5.lua gmake2
--   make -C build/gta config=release_de        (or release_classic)
local sdkdir = os.getenv("PLUGIN_SDK_DIR")
if not sdkdir or sdkdir == "" then
    error("PLUGIN_SDK_DIR is not set (absolute path to the pinned Plugin-SDK checkout)")
end
sdkdir = path.translate(sdkdir, "/")
local here = path.getabsolute(".")
local root = path.getabsolute("../..", here)
local mingw = _ACTION == "gmake" or _ACTION == "gmake2"

workspace "AzerothTheftAuto"
    location (root .. "/build/gta")
    configurations { "Release", "Debug" }
    platforms { "Classic", "DE" }

project "AzerothTheftAuto"
    kind "SharedLib"
    language "C++"
    targetextension ".asi"
    targetname "AzerothTheftAuto"
    targetprefix ""
    targetdir (root .. "/build/gta/bin/%{cfg.platform}/%{cfg.buildcfg}")
    objdir (root .. "/build/gta/obj/%{cfg.platform}/%{cfg.buildcfg}")
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
    }
    defines { "_CRT_SECURE_NO_WARNINGS", "_CRT_NON_CONFORMING_SWPRINTFS" }
    libdirs { sdkdir .. "/output/lib" }
    links { "ws2_32" }

    filter "platforms:Classic"
        architecture "x86"
        defines { "GTASA", "PLUGIN_SGV_10US", "RW" }
        includedirs {
            sdkdir .. "/plugin_sa",
            sdkdir .. "/plugin_sa/game_sa",
            sdkdir .. "/plugin_sa/game_sa/enums",
            sdkdir .. "/plugin_sa/game_sa/rw",
        }
    filter "platforms:DE"
        architecture "x86_64"
        defines { "GTASA_UNREAL", "PLUGIN_UNREAL", "UNREAL", "NOASM", "RWINT32FROMFLOAT", "_WIN64" }
        includedirs {
            sdkdir .. "/plugin_sa_unreal",
            sdkdir .. "/plugin_sa_unreal/game_sa_unreal",
            sdkdir .. "/plugin_sa_unreal/game_sa_unreal/enums",
            sdkdir .. "/plugin_sa_unreal/game_sa_unreal/rw",
        }

    filter { "platforms:Classic", "configurations:Release" }
        links { "Plugin" }
    filter { "platforms:Classic", "configurations:Debug" }
        links { mingw and "Plugin" or "Plugin_d" }
    filter { "platforms:DE", "configurations:Release" }
        links { "Plugin_Unreal" }
    filter { "platforms:DE", "configurations:Debug" }
        links { mingw and "Plugin_Unreal" or "Plugin_Unreal_d" }
    filter "configurations:Release"
        optimize "On"
    filter "configurations:Debug"
        symbols "On"
        defines "DEBUG"
    filter {}

    if mingw then
        toolset "clang"
        buildoptions {
            "-fpermissive", "-fcommon", "-fms-extensions",
            "-Wno-invalid-offsetof", "-Wno-microsoft-include", "-Wno-builtin-macro-redefined",
            "-D__cpp_concepts=202202L",
        }
        linkoptions { "-static", "-static-libgcc", "-static-libstdc++" }
        filter "platforms:Classic"
            buildoptions { "--target=i686-w64-mingw32" }
            linkoptions { "--target=i686-w64-mingw32" }
        filter "platforms:DE"
            buildoptions { "--target=x86_64-w64-mingw32" }
            linkoptions { "--target=x86_64-w64-mingw32" }
        filter {}
    else
        buildoptions { "/sdl-" }
        multiprocessorcompile "On"
    end
