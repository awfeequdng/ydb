
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(ut)

add_library(topic)
target_link_libraries(topic PUBLIC
  contrib-libs-cxxsupp
  yutil
  common
  cpp-client-ydb_proto
  cpp-client-ydb_persqueue_public
  cpp-client-ydb_topic
  tools-enum_parser-enum_serialization_runtime
)
target_sources(topic PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/ydb_cli/topic/topic_read.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/ydb_cli/topic/topic_write.cpp
)
generate_enum_serilization(topic
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/ydb_cli/topic/topic_metadata_fields.h
  INCLUDE_HEADERS
  ydb/public/lib/ydb_cli/topic/topic_metadata_fields.h
)
