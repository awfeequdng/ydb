
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-dq-task_runner)
target_compile_options(providers-dq-task_runner PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(providers-dq-task_runner PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-svnversion
  cpp-threading-task_scheduler
  cpp-yson-node
  library-yql-minikql
  yql-minikql-computation
  yql-minikql-invoke_builtins
  library-yql-protos
  library-yql-utils
  yql-utils-backtrace
  yql-utils-log
  yql-dq-proto
  yql-dq-runtime
  providers-common-proto
  dq-api-protos
  providers-dq-counters
  providers-dq-interface
)
target_sources(providers-dq-task_runner PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/task_runner/file_cache.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/task_runner/tasks_runner_local.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/task_runner/tasks_runner_proxy.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/task_runner/tasks_runner_pipe.cpp
)
