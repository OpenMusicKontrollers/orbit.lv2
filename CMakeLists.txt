cmake_minimum_required(VERSION 2.8)

project(orbit.lv2)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_C_FLAGS "-std=gnu99 -fvisibility=hidden ${CMAKE_C_FLAGS}")
add_definitions("-D_GNU_SOURCE=1") # asprintf

set(ORBIT_MAJOR_VERSION 0)
set(ORBIT_MINOR_VERSION 1)
set(ORBIT_MICRO_VERSION 0)
set(ORBIT_VERSION "${ORBIT_MAJOR_VERSION}.${ORBIT_MINOR_VERSION}.${ORBIT_MICRO_VERSION}")
add_definitions("-DORBIT_VERSION=\"${ORBIT_VERSION}\"")

set(DEST lib/lv2/orbit.lv2)
if(WIN32)
	set(LIB_EXT ".dll")
elseif(APPLE)
	set(LIB_EXT ".so")
else()
	set(LIB_EXT ".so")
endif()

find_package(PkgConfig) # ${PKG_CONFIG_FOUND}

# orbit
add_library(orbit MODULE
	orbit.c
	orbit_looper.c
	orbit_click.c
	orbit_pacemaker.c)
target_link_libraries(orbit ${LIBS})
set_target_properties(orbit PROPERTIES PREFIX "")
install(TARGETS orbit DESTINATION ${DEST})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/orbit.ttl DESTINATION ${DEST})

# manifest
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/manifest.ttl.in ${PROJECT_BINARY_DIR}/manifest.ttl)
install(FILES ${PROJECT_BINARY_DIR}/manifest.ttl DESTINATION ${DEST})
