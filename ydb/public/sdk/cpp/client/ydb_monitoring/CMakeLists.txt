
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-client-ydb_monitoring)
target_link_libraries(cpp-client-ydb_monitoring PUBLIC
  contrib-libs-cxxsupp
  yutil
  tools-enum_parser-enum_serialization_runtime
  cpp-client-ydb_proto
  impl-ydb_internal-make_request
  client-ydb_common_client-impl
  cpp-client-ydb_driver
)
target_sources(cpp-client-ydb_monitoring PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_monitoring/monitoring.cpp
)
generate_enum_serilization(cpp-client-ydb_monitoring
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_monitoring/monitoring.h
  INCLUDE_HEADERS
  ydb/public/sdk/cpp/client/ydb_monitoring/monitoring.h
)
