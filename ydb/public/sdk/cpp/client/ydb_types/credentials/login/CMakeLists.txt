
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(ydb_types-credentials-login)
target_link_libraries(ydb_types-credentials-login PUBLIC
  contrib-libs-cxxsupp
  yutil
  ydb-library-login
  api-grpc
  client-ydb_types-status
  impl-ydb_internal-grpc_connections
  yql-public-issue
)
target_sources(ydb_types-credentials-login PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/sdk/cpp/client/ydb_types/credentials/login/login.cpp
)
