
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(exec)

add_library(providers-dq-provider)
target_compile_options(providers-dq-provider PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(providers-dq-provider PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-grpc-client
  cpp-threading-task_scheduler
  public-lib-yson_value
  cpp-client-ydb_driver
  library-yql-core
  yql-dq-transform
  yql-dq-tasks
  yql-dq-type_ann
  providers-common-gateway
  providers-common-metrics
  common-schema-expr
  providers-common-transform
  dq-api-grpc
  dq-api-protos
  providers-dq-backtrace
  providers-dq-common
  providers-dq-config
  providers-dq-expr_nodes
  providers-dq-interface
  providers-dq-opt
  providers-dq-planner
  providers-result-expr_nodes
  library-yql-minikql
)
target_sources(providers-dq-provider PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_control.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_datasink_type_ann.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_datasource_type_ann.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_gateway.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_provider.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_datasink.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_datasource.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/provider/yql_dq_recapture.cpp
)
