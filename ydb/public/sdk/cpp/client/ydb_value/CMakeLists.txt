
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(ut)

add_library(cpp-client-ydb_value)
target_link_libraries(cpp-client-ydb_value PUBLIC
  contrib-libs-cxxsupp
  yutil
  tools-enum_parser-enum_serialization_runtime
  cpp-containers-stack_vector
  api-protos
  impl-ydb_internal-value_helpers
  cpp-client-ydb_proto
  client-ydb_types-fatal_error_handlers
  yql-public-decimal
)
target_sources(cpp-client-ydb_value PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_value/value.cpp
)
generate_enum_serilization(cpp-client-ydb_value
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_value/value.h
  INCLUDE_HEADERS
  ydb/public/sdk/cpp/client/ydb_value/value.h
)
