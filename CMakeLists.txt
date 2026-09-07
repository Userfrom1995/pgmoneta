cmake_minimum_required(VERSION 3.14)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "22")
set(VERSION_PATCH "0")
set(VERSION_STRING ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

#
# Avoid source tree pollution
#
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not permitted. Make a separate folder for building:\nmkdir build; cd build; cmake ..\nBefore that, remove the files already created:\nrm -rf CMakeCache.txt CMakeFiles")
endif(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)

project(pgmoneta VERSION ${VERSION_STRING} LANGUAGES  C)

include(CTest)
enable_testing()

set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME
  "${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set(CPACK_SOURCE_IGNORE_FILES
  "/build/;/test/;/.cache/;/.git/;/.github/;/*.patch;/.bundle/;/_site/;/vendor/;~$;${CPACK_SOURCE_IGNORE_FILES}")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
include(CPack)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "pgmoneta ${VERSION_STRING}")

if (NOT DEFINED DOCS)
  set(DOCS TRUE)
endif()
set(check TRUE)
set(container FALSE)

include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CheckLinkerFlag)
include(FindPackageHandleStandardArgs)
include(GNUInstallDirs)
include(CheckIncludeFile)
include(CheckSymbolExists)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build, options are: Debug Release" FORCE)
endif ()

message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")
message(STATUS "System is ${CMAKE_SYSTEM_NAME}")

set(SUPPORTED_COMPILERS "GNU" "Clang" "AppleClang")

# Check for a supported compiler
if (NOT CMAKE_C_COMPILER_ID IN_LIST SUPPORTED_COMPILERS)
   message(FATAL_ERROR "Unsupported compiler ${CMAKE_C_COMPILER_ID}. Supported compilers are: ${SUPPORTED_COMPILERS}")
endif ()

CHECK_C_COMPILER_FLAG("-std=c17" COMPILER_SUPPORTS_C17)
if(NOT COMPILER_SUPPORTS_C17)
  message(FATAL_ERROR "The compiler ${CMAKE_C_COMPILER} has no C17 support. Please use a different C compiler.")
endif()

CHECK_C_COMPILER_FLAG("-msse4.2" COMPILER_SUPPORTS_SSE42)
if(NOT COMPILER_SUPPORTS_SSE42)
  message(NOTICE "The compiler ${CMAKE_C_COMPILER} has no SSE4.2 support.")
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  CHECK_C_COMPILER_FLAG(-msse4.2 HAVE_CRC32_SSE42)
  if (${HAVE_CRC32_SSE42})
    message(STATUS "CPU have -msse4.2, defined HAVE_CRC32_SSE42")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAVE_CRC32_SSE42 -msse4.2")
  endif ()
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  CHECK_C_COMPILER_FLAG(-march=armv8.1-a HAVE_CRC32_HARDWARE)
  if (${HAVE_CRC32_HARDWARE})
    message(STATUS "CPU have -march=armv8.1-a, defined HAVE_CRC32C")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAVE_CRC32C -march=armv8.1-a")
  endif ()
endif ()

if(NOT HAVE_CRC32_SSE42)
  message(STATUS "CRC32C implementation will use the software version")
elseif(HAVE_CRC32_SSE42)
  message(STATUS "CRC32C implementation will use SSE 4.2")
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    # Homebrew ships libarchive keg only, include dirs have to be set manually
    execute_process(
      COMMAND brew --prefix libarchive
      OUTPUT_VARIABLE LIBARCHIVE_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
    )
    set(LibArchive_INCLUDE_DIR "${LIBARCHIVE_PREFIX}/include")

    execute_process(
        COMMAND brew --prefix pandoc
        OUTPUT_VARIABLE PANDOC_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )

endif()

find_package(Check)
if (CHECK_FOUND)
  message(STATUS "check found")
  add_library(Check::check SHARED IMPORTED)
  set_target_properties(Check::check PROPERTIES
    IMPORTED_LOCATION ${CHECK_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${CHECK_INCLUDE_DIR})
else ()
  set(check FALSE)
  message(STATUS "check needed. The test suite process will be skipped.")
endif()

find_package(Docker)
find_package(Podman)
if (DOCKER_FOUND OR PODMAN_FOUND)
  set(container TRUE)
  message(STATUS "Docker or podman found")
else ()
  message(STATUS "Docker or podman needed. The test suite will be skipped.")
endif()

if (DOCS)
  find_package(Pandoc)
  if (PANDOC_FOUND)
    message(STATUS "pandoc found")
  else ()
    set(DOCS FALSE)
    message(STATUS "pandoc needed. The generation process will be skipped.")
  endif()

  find_package(Xelatex)
  if (XELATEX_FOUND)
    message(STATUS "xelatex found")
  else ()
    set(DOCS FALSE)
    message(STATUS "xelatex needed. The generation process will be skipped.")
  endif()
endif()

find_package(ZLIB)
if (ZLIB_FOUND)
  message(STATUS "zlib found")
else ()
  message(FATAL_ERROR "zlib needed")
endif()

find_package(BZip2)
if (BZIP2_FOUND)
  message(STATUS "bzip2 found")
else ()
  message(FATAL_ERROR "bzip2 needed")
endif()

find_package(Zstd)
if (ZSTD_FOUND)
  message(STATUS "zstd found")
else ()
  message(FATAL_ERROR "zstd needed")
endif()

find_package(Lz4)
if (LZ4_FOUND)
  message(STATUS "lz4 found")
else ()
  message(FATAL_ERROR "lz4 needed")
endif()

#
# Event backend detection (io_uring/epoll/kqueue); custom layer in src/include/ev.h
#
if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  # Check for io_uring support (liburing >= 2.5)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBURING liburing>=2.5)
  
  if (LIBURING_FOUND)
    set(HAVE_IO_URING 1 CACHE BOOL "io_uring support available")
    message(STATUS "io_uring found (liburing ${LIBURING_VERSION})")
    
    # Check for required io_uring constants
    set(CMAKE_REQUIRED_INCLUDES "${LIBURING_INCLUDE_DIRS}")
    set(HAVE_IO_URING_CONSTANTS ON)
    set(MISSING_IO_URING_CONSTANTS "")
    
    foreach(constant
            IORING_SETUP_CQSIZE
            IORING_SETUP_DEFER_TASKRUN
            IORING_SETUP_SINGLE_ISSUER
            IORING_TIMEOUT_MULTISHOT
            IORING_SQ_CQ_OVERFLOW
            IORING_CQE_BUFFER_SHIFT
            IORING_CQE_F_MORE)
      set(IO_URING_CONSTANTS_TEST "
#include <liburing.h>
int main(void) {
  (void)${constant};
  return 0;
}
")
      set(const_var "HAVE_${constant}")
      check_c_source_compiles("${IO_URING_CONSTANTS_TEST}" ${const_var})
      if(${const_var})
        message(STATUS "io_uring constant check: ${constant}=usable")
      else()
        message(STATUS "io_uring constant check: ${constant}=not-usable")
        set(HAVE_IO_URING_CONSTANTS OFF)
        list(APPEND MISSING_IO_URING_CONSTANTS "${constant}")
      endif()
    endforeach()
    set(CMAKE_REQUIRED_INCLUDES "")
    
    if (NOT HAVE_IO_URING_CONSTANTS)
      message(STATUS "io_uring constants missing; falling back to epoll")
      message(STATUS "Missing io_uring constants: ${MISSING_IO_URING_CONSTANTS}")
      set(HAVE_IO_URING 0 CACHE BOOL "io_uring support available" FORCE)
    endif()
  else ()
    set(HAVE_IO_URING 0 CACHE BOOL "io_uring support available")
    message(STATUS "io_uring not found (liburing >= 2.5 required)")
  endif ()
  
  # Check for epoll support
  include(CheckSymbolExists)
  check_symbol_exists(epoll_create1 "sys/epoll.h" HAVE_EPOLL)
  if (HAVE_EPOLL)
    message(STATUS "epoll found")
  else ()
    message(STATUS "epoll not available")
  endif ()
  
  # Check for epoll_pwait2 (Linux 5.11+)
  check_symbol_exists(epoll_pwait2 "sys/epoll.h" HAVE_EPOLL_PWAIT2)
  if (HAVE_EPOLL_PWAIT2)
    message(STATUS "epoll_pwait2 found")
  endif ()
  
  set(HAVE_LINUX 1 CACHE BOOL "Linux system")
  
  # Check Linux kernel version (simplified check)
  execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE KERNEL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE KERNEL_RESULT
  )
  if (KERNEL_RESULT EQUAL 0)
    message(STATUS "Linux kernel version: ${KERNEL_VERSION}")
    # Basic version check - io_uring requires 5.1+
    if (KERNEL_VERSION VERSION_GREATER_EQUAL "5.1")
      message(STATUS "Kernel supports io_uring (5.1+)")
    else ()
      message(STATUS "Kernel may not support io_uring (requires 5.1+)")
    endif ()
  endif ()
  
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD" OR ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
  # Check for kqueue support (BSD/macOS)
  check_symbol_exists(kqueue "sys/event.h" HAVE_KQUEUE)
  if (HAVE_KQUEUE)
    message(STATUS "kqueue found")
  else ()
    message(FATAL_ERROR "kqueue not available")
  endif ()
endif ()

find_package(OpenSSL)
if (OPENSSL_FOUND)
  message(STATUS "OpenSSL found")
else ()
  message(FATAL_ERROR "OpenSSL needed")
endif()

find_package(LibArchive)
if (LibArchive_FOUND)
  message(STATUS "libarchive found")
else ()
  message(FATAL_ERROR "libarchive needed")
endif()


find_package(Rst2man)
if (RST2MAN_FOUND)
  message(STATUS "rst2man found")
else ()
  message(FATAL_ERROR "rst2man needed")
endif()


find_package(Libssh)
if (LIBSSH_FOUND)
  message(STATUS "libssh found")
else ()
  message(FATAL_ERROR "libssh needed")
endif()

find_package(LibYAML)
if (LIBYAML_FOUND)
  message(STATUS "LibYAML found")
else ()
  message(FATAL_ERROR "LibYAML needed")
endif()

find_package(THREAD)
if (THREAD_FOUND)
  message(STATUS "pthread found")
else ()
  message(FATAL_ERROR "pthread needed")
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  find_package(Libatomic)
  if (LIBATOMIC_FOUND)
    message(STATUS "libatomic found")
  else ()
    message(FATAL_ERROR "libatomic needed")
  endif()

  find_package(Systemd)
  if (SYSTEMD_FOUND)
    message(STATUS "systemd found")
  else ()
    message(STATUS "systemd not found; building without systemd support")
  endif()
endif()

find_package(Doxygen)

if (DOXYGEN_FOUND)
  message(status "Doxygen found: ${DOXYGEN_EXECUTABLE}")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/")

add_subdirectory(doc)
add_subdirectory(src)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test")
  add_subdirectory(test)
else()
  message(STATUS "Test directory not found, skipping tests")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks")
  add_subdirectory(benchmarks)
endif()
