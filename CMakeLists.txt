cmake_minimum_required(VERSION 3.16)

# ── force MinGW g++ on Windows before project() detects compilers ─────────
if(WIN32)
    if(EXISTS "C:/ProgramData/mingw64/mingw64/bin/g++.exe")
        set(CMAKE_C_COMPILER   "C:/ProgramData/mingw64/mingw64/bin/gcc.exe" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER "C:/ProgramData/mingw64/mingw64/bin/g++.exe" CACHE STRING "" FORCE)
    endif()
endif()

project(novac CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── compiler flags ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /EHsc /utf-8)
else()
    add_compile_options(
        -O2
        -fcoroutines
        -fexec-charset=UTF-8
        -finput-charset=UTF-8
    )
endif()

# ════════════════════════════════════════════════════════════════════════════
#  platform libs
# ════════════════════════════════════════════════════════════════════════════

if(WIN32)
    # Support local MinGW, CI MSYS2 MinGW64, and MSYS2 clangarm64 (Windows ARM64)
    if(EXISTS "C:/msys64/clangarm64/include")
        set(MINGW_LIB "C:/msys64/clangarm64/lib")
        set(MINGW_INC "C:/msys64/clangarm64/include")
    elseif(EXISTS "C:/tools/msys64/mingw64/include")
        set(MINGW_LIB "C:/tools/msys64/mingw64/lib")
        set(MINGW_INC "C:/tools/msys64/mingw64/include")
    elseif(EXISTS "C:/ProgramData/mingw64/mingw64/include")
        set(MINGW_LIB "C:/ProgramData/mingw64/mingw64/lib")
        set(MINGW_INC "C:/ProgramData/mingw64/mingw64/include")
    else()
        set(MINGW_LIB "")
        set(MINGW_INC "")
    endif()
    set(PLATFORM_INCLUDE_DIRS ${MINGW_INC})
    set(PLATFORM_LINK_DIRS    ${MINGW_LIB})
    set(PLATFORM_LIBS         ws2_32 secur32 ffi ssl crypto ole32 oleaut32 uuid user32 gdi32 comctl32 comdlg32)
elseif(APPLE)
    find_package(PkgConfig QUIET)
    # Homebrew OpenSSL (arm64: /opt/homebrew, x86: /usr/local)
    if(EXISTS "/opt/homebrew/opt/openssl")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
    elseif(EXISTS "/usr/local/opt/openssl")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
    endif()
    find_package(OpenSSL QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFI libffi)
    endif()
    if(FFI_FOUND)
        set(PLATFORM_INCLUDE_DIRS ${FFI_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        set(PLATFORM_LINK_DIRS    "")
        set(PLATFORM_LIBS         ${FFI_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto "-framework Security" "-framework CoreFoundation")
    else()
        set(PLATFORM_INCLUDE_DIRS /usr/local/include /opt/homebrew/include ${OPENSSL_INCLUDE_DIR})
        set(PLATFORM_LINK_DIRS    /usr/local/lib /opt/homebrew/lib)
        set(PLATFORM_LIBS         ffi OpenSSL::SSL OpenSSL::Crypto "-framework Security" "-framework CoreFoundation")
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFI REQUIRED libffi)
    find_package(OpenSSL REQUIRED)
    set(PLATFORM_INCLUDE_DIRS ${FFI_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
    set(PLATFORM_LINK_DIRS    "")
    set(PLATFORM_LIBS         ${FFI_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
endif()

# ════════════════════════════════════════════════════════════════════════════
#  novac
# ════════════════════════════════════════════════════════════════════════════

set(NOVAC_SOURCES
    src/lex/lexer.cpp
    src/parse/parser.cpp
    src/runtime/scope.cpp
    src/runtime/type_registry.cpp
    src/runtime/fetch.cpp
    src/runtime/bstd.cpp
    src/vm/vm.cpp
    src/cli/cli.cpp
    src/cli/cli_dump.cpp
    src/cli/cli_misc.cpp
    bin/main.cpp
)

add_executable(novac ${NOVAC_SOURCES})

target_include_directories(novac PRIVATE
    src
    .
    ${PLATFORM_INCLUDE_DIRS}
)

target_compile_definitions(novac PRIVATE NOVA_HAVE_LIBFFI)

target_compile_definitions(novac PRIVATE NOVA_HAVE_LIBFFI UNICODE _UNICODE)
if(WIN32)
    target_compile_definitions(novac PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(PLATFORM_LINK_DIRS)
    target_link_directories(novac PRIVATE ${PLATFORM_LINK_DIRS})
endif()

target_link_libraries(novac PRIVATE ${PLATFORM_LIBS})

# ════════════════════════════════════════════════════════════════════════════
#  nv+
# ════════════════════════════════════════════════════════════════════════════

add_executable(nvplus bin/nv+.cpp)

target_include_directories(nvplus PRIVATE
    src
    .
    ${PLATFORM_INCLUDE_DIRS}
)

target_compile_definitions(nvplus PRIVATE NOVA_HAVE_LIBFFI)

target_compile_definitions(nvplus PRIVATE NOVA_HAVE_LIBFFI UNICODE _UNICODE)
if(WIN32)
    target_compile_definitions(nvplus PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(PLATFORM_LINK_DIRS)
    target_link_directories(nvplus PRIVATE ${PLATFORM_LINK_DIRS})
endif()

if(WIN32)
    # ole32/oleaut32/uuid added for Std.Windows.COM, matching the novac target
    target_link_libraries(nvplus PRIVATE ws2_32 secur32 wininet ffi ole32 oleaut32 uuid user32 gdi32 comctl32 comdlg32)
else()
    target_link_libraries(nvplus PRIVATE ${PLATFORM_LIBS})
endif()

# output as nv+ not nvplus
set_target_properties(nvplus PROPERTIES OUTPUT_NAME "nv+")

# ════════════════════════════════════════════════════════════════════════════
#  install
# ════════════════════════════════════════════════════════════════════════════

install(TARGETS novac nvplus DESTINATION bin)

# ════════════════════════════════════════════════════════════════════════════
#  summary
# ════════════════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "novac build configuration:")
message(STATUS "  Platform : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Targets  : novac, nv+")
message(STATUS "")
