
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-dq-common)
target_compile_options(providers-dq-common PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(providers-dq-common PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-string_utils-parse_size
  library-yql-minikql
  library-yql-sql
  library-yql-utils
  yql-utils-log
  yql-dq-actors
)
target_sources(providers-dq-common PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/common/attrs.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/common/yql_dq_common.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/common/yql_dq_settings.cpp
)
