
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(dq-actors-task_runner)
target_compile_options(dq-actors-task_runner PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(dq-actors-task_runner PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-core
  yql-dq-runtime
  yql-dq-common
  yql-dq-proto
  library-yql-minikql
  yql-minikql-computation
  yql-utils-actors
  ydb-core-protos
)
target_sources(dq-actors-task_runner PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/dq/actors/task_runner/events.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/dq/actors/task_runner/task_runner_actor_local.cpp
)
