
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-pq-cm_client)
target_link_libraries(providers-pq-cm_client PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-threading-future
  yql-public-issue
  client-ydb_types-credentials
  tools-enum_parser-enum_serialization_runtime
)
target_sources(providers-pq-cm_client PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/pq/cm_client/client.cpp
)
generate_enum_serilization(providers-pq-cm_client
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/pq/cm_client/client.h
  INCLUDE_HEADERS
  ydb/library/yql/providers/pq/cm_client/client.h
)
