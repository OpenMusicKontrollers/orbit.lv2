cmake_minimum_required(VERSION 2.8)

project(orbit.lv2)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/timely.lv2)
include_directories(${PROJECT_SOURCE_DIR}/props.lv2)
include_directories(${PROJECT_SOURCE_DIR}/varchunk)
include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_C_FLAGS "-fdata-sections -ffunction-sections ${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "-std=gnu11 -Wextra -Wno-unused-parameter -ffast-math -fvisibility=hidden ${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "-Wshadow -Wimplicit-function-declaration -Wredundant-decls -Wmissing-prototypes -Wstrict-prototypes -Wno-attributes ${CMAKE_C_FLAGS}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(CMAKE_MODULE_LINKER_FLAGS "-Wl,-z,defs ${CMAKE_MODULE_LINKER_FLAGS}")
	set(CMAKE_MODULE_LINKER_FLAGS "-Wl,-z,nodelete ${CMAKE_MODULE_LINKER_FLAGS}")
elseif(WIN32)
	set(CMAKE_C_FLAGS "-mstackrealign ${CMAKE_C_FLAGS}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	if(APPLE)
		set(CMAKE_MODULE_LINKER_FLAGS "-Wl,-dead_strip ${CMAKE_MODULE_LINKER_FLAGS}")
	else()
		set(CMAKE_MODULE_LINKER_FLAGS "-Wl,--gc-sections -Wl,-s ${CMAKE_MODULE_LINKER_FLAGS}")
	endif()
endif()

add_definitions("-D_GNU_SOURCE=1") # asprintf

file(STRINGS "VERSION" ORBIT_VERSION)
string(REPLACE "." ";" VERSION_LIST ${ORBIT_VERSION})
list(GET VERSION_LIST 0 ORBIT_MAJOR_VERSION)
list(GET VERSION_LIST 1 ORBIT_MINOR_VERSION)
list(GET VERSION_LIST 2 ORBIT_MICRO_VERSION)
add_definitions("-DORBIT_VERSION=\"${ORBIT_VERSION}\"")

if(NOT DEFINED PLUGIN_DEST)
	set(PLUGIN_DEST lib/lv2/orbit.lv2)
endif()

find_package(PkgConfig) # ${PKG_CONFIG_FOUND}

pkg_search_module(LV2 REQUIRED lv2)
include_directories(${LV2_INCLUDE_DIRS})

#pkg_search_module(ZLIB REQUIRED zlib)
#include_directories(${ZLIB_INCLUDE_DIRS})
#set(LIBS ${LIBS} ${ZLIB_LDFLAGS})

# orbit
add_library(orbit MODULE
	orbit.c
	orbit_looper.c
	orbit_click.c
	orbit_pacemaker.c
	orbit_beatbox.c
	orbit_subspace.c
	orbit_cargoship.c
	orbit_quantum.c)
target_link_libraries(orbit m ${LIBS})
set_target_properties(orbit PROPERTIES PREFIX "")
install(TARGETS orbit DESTINATION ${PLUGIN_DEST})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/orbit.ttl DESTINATION ${PLUGIN_DEST})

# manifest
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/manifest.ttl.in ${PROJECT_BINARY_DIR}/manifest.ttl)
install(FILES ${PROJECT_BINARY_DIR}/manifest.ttl DESTINATION ${PLUGIN_DEST})
