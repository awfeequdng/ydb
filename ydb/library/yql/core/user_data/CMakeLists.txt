
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yql-core-user_data)
target_link_libraries(yql-core-user_data PUBLIC
  contrib-libs-cxxsupp
  yutil
  tools-enum_parser-enum_serialization_runtime
)
target_sources(yql-core-user_data PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/core/user_data/yql_user_data.cpp
)
generate_enum_serilization(yql-core-user_data
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/core/user_data/yql_user_data.h
  INCLUDE_HEADERS
  ydb/library/yql/core/user_data/yql_user_data.h
)
