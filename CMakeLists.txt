cmake_minimum_required(VERSION 3.25)
project(vdetelweb
    VERSION 1.2.3
    DESCRIPTION "Telnet and WEB interface for VDE2"
    HOMEPAGE_URL "https://github.com/virtualsquare/vdetelweb"
    LANGUAGES C)

include(GNUInstallDirs)
include(CheckIncludeFile)
include(CheckSymbolExists)

add_definitions(-D_GNU_SOURCE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FORTIFY_SOURCE=2 -O2 -pedantic -Wall -Wextra --std=gnu17")

set(LIBS_REQUIRED lwipv6 vdehist mhash)
set(HEADERS_REQUIRED lwipv6.h libvdehist.h mhash.h)

foreach(THISLIB IN LISTS LIBS_REQUIRED)
  find_library(LIB${THISLIB}_OK ${THISLIB})
  if(NOT LIB${THISLIB}_OK)
    message(FATAL_ERROR "library lib${THISLIB} not found")
  endif()
endforeach(THISLIB)

foreach(HEADER IN LISTS HEADERS_REQUIRED)
  check_include_file(${HEADER} ${HEADER}_OK "-include stdlib.h")
  if(NOT ${HEADER}_OK)
    message(FATAL_ERROR "header file ${HEADER} not found")
  endif()
endforeach(HEADER)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(vdetelweb vdetelweb.c web.c telnet.c)
target_include_directories(vdetelweb PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(vdetelweb lwipv6 vdehist mhash)

configure_file(config.h.in config.h)

install(TARGETS vdetelweb
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES vdetelweb.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

add_custom_target(uninstall
  "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/Uninstall.cmake")

