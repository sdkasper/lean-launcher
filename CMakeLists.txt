cmake_minimum_required(VERSION 3.21)
project(LeanLauncher VERSION 1.5.0 LANGUAGES CXX)

function(add_launcher target)
add_executable(${target} WIN32
    src/main.cpp
    src/launcher.h
    src/file_index.h
    src/search.h
    src/settings.h
    src/updates.h
    src/calculator.h
    src/obsidian_config.h
    src/daily_note.h
    src/note_index.h
    src/LeanLauncher.manifest
    src/LeanLauncher.rc
)

target_compile_features(${target} PRIVATE cxx_std_17)
target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /sdl)
target_link_libraries(${target} PRIVATE
    dwmapi
    shell32
    shlwapi
    user32
    gdi32
    ole32
    d2d1
    dwrite
    windowscodecs
    imm32
    advapi32
    winhttp
)

set_target_properties(${target} PROPERTIES
    VS_GLOBAL_EnableCppCoreCheck false
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
)
# Match LeanLauncher.vcxproj's Release settings (WholeProgramOptimization +
# EnableCOMDATFolding/OptimizeReferences) so the CMake and MSBuild build
# paths produce the same size binary - CMake's own Release defaults don't
# enable whole-program optimization or linker dead-code/COMDAT folding.
target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endfunction()

add_launcher(LeanLauncher)

include(CTest)
if(BUILD_TESTING)
    add_executable(LeanLauncherCoreTests tests/core_tests.cpp)
    target_compile_features(LeanLauncherCoreTests PRIVATE cxx_std_17)
    target_compile_options(LeanLauncherCoreTests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(LeanLauncherCoreTests PRIVATE winhttp ole32 shell32 shlwapi)
    add_test(NAME LeanLauncherCoreTests COMMAND LeanLauncherCoreTests)

    # Separate class, mutex, fixture apps, and no global shortcut. Never attaches
    # to or replaces a user's running launcher.
    add_launcher(LeanLauncherUiTests)
    target_compile_definitions(LeanLauncherUiTests PRIVATE LEANLAUNCHER_UI_TEST)
endif()
