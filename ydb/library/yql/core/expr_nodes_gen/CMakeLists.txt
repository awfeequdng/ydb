
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yql-core-expr_nodes_gen)
target_link_libraries(yql-core-expr_nodes_gen PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-yql-ast
  yql-public-udf
)
target_sources(yql-core-expr_nodes_gen PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/core/expr_nodes_gen/yql_expr_nodes_gen.cpp
)
