cmake_minimum_required(VERSION 3.25)
project(WUMFix VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "WUMFix must be built for 32-bit x86 (WormsMayhem.exe is a 32-bit process). Use build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the .asi has no runtime DLL dependencies on players' machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
set(SAFETYHOOK_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(safetyhook)

# Every .cpp under src/ is compiled into the plugin. Adding a module = dropping a file in src/modules/.
file(GLOB_RECURSE WUMFIX_SOURCES CONFIGURE_DEPENDS src/*.cpp)
configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

add_library(WUMFix SHARED ${WUMFIX_SOURCES} src/core/version.rc)
set_target_properties(WUMFix PROPERTIES SUFFIX ".asi" PREFIX "")
target_include_directories(WUMFix PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(WUMFix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(WUMFix PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(WUMFix PRIVATE /DEBUG /OPT:REF /OPT:ICF /SAFESEH:NO)
target_link_libraries(WUMFix PRIVATE safetyhook::safetyhook dbghelp bcrypt ws2_32)
