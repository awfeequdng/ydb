
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(core-testlib-basics)
target_compile_options(core-testlib-basics PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(core-testlib-basics PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-dnsresolver
  cpp-regex-pcre
  cpp-testing-unittest
  ydb-core-base
  ydb-core-blobstorage
  core-blobstorage-crypto
  core-blobstorage-nodewarden
  core-blobstorage-pdisk
  core-client-server
  ydb-core-formats
  ydb-core-mind
  ydb-core-node_whiteboard
  ydb-core-quoter
  ydb-core-tablet_flat
  core-testlib-actors
  core-tx-columnshard
  core-tx-scheme_board
  ydb-core-util
  yql-minikql-invoke_builtins
  udf-service-exception_policy
  yql-sql-pg_dummy
)
target_sources(core-testlib-basics PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/testlib/basics/appdata.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/testlib/basics/helpers.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/testlib/basics/runtime.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/testlib/basics/services.cpp
)
