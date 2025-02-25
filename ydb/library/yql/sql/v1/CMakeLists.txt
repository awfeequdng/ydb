
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(format)
add_subdirectory(lexer)
add_subdirectory(perf)
add_subdirectory(proto_parser)

add_library(yql-sql-v1)
target_compile_options(yql-sql-v1 PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(yql-sql-v1 PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-charset
  library-cpp-enumbitset
  cpp-yson-node
  library-cpp-json
  yql-core-sql_types
  library-yql-minikql
  yql-public-udf
  yql-sql-settings
  library-yql-core
  yql-core-issue
  core-issue-protos
  yql-parser-lexer_common
  yql-parser-proto_ast
  parser-proto_ast-collect_issues
  proto_ast-gen-v1
  proto_ast-gen-v1_ansi
  proto_ast-gen-v1_proto
  yql-parser-pg_catalog
  sql-v1-lexer
  sql-v1-proto_parser
  tools-enum_parser-enum_serialization_runtime
)
target_sources(yql-sql-v1 PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/aggregation.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/builtin.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/context.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/join.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/insert.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/list_builtin.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/node.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/select.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/sql.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/query.cpp
)
generate_enum_serilization(yql-sql-v1
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/node.h
  INCLUDE_HEADERS
  ydb/library/yql/sql/v1/node.h
)
generate_enum_serilization(yql-sql-v1
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/sql/v1/sql_call_param.h
  INCLUDE_HEADERS
  ydb/library/yql/sql/v1/sql_call_param.h
)
