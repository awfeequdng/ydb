
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(arrow)
add_subdirectory(benchmark)
add_subdirectory(codegen)
add_subdirectory(comp_nodes)
add_subdirectory(computation)
add_subdirectory(dom)
add_subdirectory(invoke_builtins)
add_subdirectory(jsonpath)
add_subdirectory(perf)
add_subdirectory(ut)

add_library(library-yql-minikql)
target_compile_options(library-yql-minikql PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(library-yql-minikql PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-apache-arrow
  libs-cctz-tzdata
  cpp-actors-util
  cpp-deprecated-enum_codegen
  library-cpp-enumbitset
  cpp-monlib-dynamic_counters
  library-cpp-packedtypes
  library-cpp-resource
  library-cpp-yson
  ydb-library-binary_json
  ydb-library-dynumber
  yql-minikql-dom
  yql-parser-pg_catalog
  yql-public-udf
  public-udf-tz
  library-yql-utils
)
target_sources(library-yql-minikql PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/aligned_page_pool.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/compact_hash.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_alloc.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_function_registry.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node_builder.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node_cast.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node_printer.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node_serialization.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_node_visitor.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_opt_literal.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_program_builder.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_runtime_version.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_stats_registry.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_string_util.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_terminator.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_type_builder.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_type_ops.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/mkql_unboxed_value_stream.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/pack_num.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/primes.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/watermark_tracker.cpp
)
