cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME dodo)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/extension ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

set(EXTENSION_SOURCES src/extension/dodo_extension.cpp src/core/dodo_core.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# CLI tool (standalone, no DuckDB dependency)
add_executable(dodoc src/cli/dodoc.cpp src/core/dodo_core.cpp)
target_include_directories(dodoc PRIVATE src/core)
target_compile_features(dodoc PRIVATE cxx_std_17)
