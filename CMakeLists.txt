cmake_minimum_required(VERSION 3.20)
project(devbg_coroutine_echo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Exported so clangd knows this is C++20. Without it the editor flags
# std::coroutine_handle as missing, which is an editor problem, not a code one.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# echo: the demo. naive: the same file with the buffer moved out of the frame.
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED liburing)
    find_package(Threads REQUIRED)
endif()

foreach(prog echo naive)
    add_executable(${prog} src/${prog}.cpp)
    if(WIN32)
        target_link_libraries(${prog} PRIVATE ws2_32)
    else()
        target_include_directories(${prog} PRIVATE ${URING_INCLUDE_DIRS})
        target_link_libraries(${prog} PRIVATE ${URING_LIBRARIES} Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(${prog} PRIVATE /W4)
    else()
        target_compile_options(${prog} PRIVATE -Wall -Wextra)
    endif()
endforeach()
