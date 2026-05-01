cmake_minimum_required(VERSION 3.10)
project(pcitop C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -g")

execute_process(COMMAND git rev-list --count HEAD OUTPUT_VARIABLE GIT_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git rev-parse --short HEAD OUTPUT_VARIABLE GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
add_definitions("-DGIT_BUILD=\"${GIT_BUILD}\"")
add_definitions("-DGIT_VERSION=\"${GIT_VERSION}\"")

find_package(PkgConfig REQUIRED)
pkg_check_modules(PCI REQUIRED libpci)
pkg_check_modules(NCURSES REQUIRED ncurses)

include_directories(${PCI_INCLUDE_DIRS} ${NCURSES_INCLUDE_DIRS})
link_directories(${PCI_LIBRARY_DIRS} ${NCURSES_LIBRARY_DIRS})

add_executable(pcitop main.c pci_tree.c throughput.c)
target_link_libraries(pcitop ${PCI_LIBRARIES} ${NCURSES_LIBRARIES} tinfo)
