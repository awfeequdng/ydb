
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(ut)

add_library(ydb-library-binary_json)
target_compile_options(ydb-library-binary_json PRIVATE
  -DUDF_ABI_VERSION_MAJOR=2
  -DUDF_ABI_VERSION_MINOR=21
  -DUDF_ABI_VERSION_PATCH=0
)
target_link_libraries(ydb-library-binary_json PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-json
  yql-minikql-dom
  tools-enum_parser-enum_serialization_runtime
)
target_sources(ydb-library-binary_json PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/binary_json/format.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/binary_json/read.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/binary_json/write.cpp
)
generate_enum_serilization(ydb-library-binary_json
  ${CMAKE_SOURCE_DIR}/ydb/library/binary_json/format.h
  INCLUDE_HEADERS
  ydb/library/binary_json/format.h
)
