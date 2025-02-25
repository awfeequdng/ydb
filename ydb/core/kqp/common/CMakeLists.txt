
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(core-kqp-common)
target_compile_options(core-kqp-common PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(core-kqp-common PUBLIC
  contrib-libs-cxxsupp
  yutil
  ydb-core-base
  ydb-core-engine
  core-kqp-expr_nodes
  core-kqp-provider
  tx-long_tx_service-public
  ydb-library-aclib
  yql-core-issue
  yql-dq-actors
  yql-dq-common
  library-cpp-lwtrace
  tools-enum_parser-enum_serialization_runtime
)
target_sources(core-kqp-common PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_common.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_resolve.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_ru_calc.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_transform.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_yql.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_timeouts.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_lwtrace_probes.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_topic.cpp
)
generate_enum_serilization(core-kqp-common
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_tx_info.h
  INCLUDE_HEADERS
  ydb/core/kqp/common/kqp_tx_info.h
)
generate_enum_serilization(core-kqp-common
  ${CMAKE_SOURCE_DIR}/ydb/core/kqp/common/kqp_yql.h
  INCLUDE_HEADERS
  ydb/core/kqp/common/kqp_yql.h
)
