
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yq-libs-logs)
target_compile_options(yq-libs-logs PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(yq-libs-logs PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-core
  ydb-core-protos
  yql-utils-actor_log
)
target_sources(yq-libs-logs PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/yq/libs/logs/log.cpp
)
