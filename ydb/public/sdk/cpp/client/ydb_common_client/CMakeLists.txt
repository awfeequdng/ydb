
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(impl)

add_library(cpp-client-ydb_common_client)
target_link_libraries(cpp-client-ydb_common_client PUBLIC
  contrib-libs-cxxsupp
  yutil
  impl-ydb_internal-common
  client-ydb_types-credentials
)
target_sources(cpp-client-ydb_common_client PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_common_client/settings.cpp
)
