
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-dq-backtrace)
target_compile_options(providers-dq-backtrace PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(providers-dq-backtrace PUBLIC
  contrib-libs-cxxsupp
  yutil
  yql-utils-backtrace
  cpp-deprecated-atomic
)
target_sources(providers-dq-backtrace PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/backtrace/backtrace.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/dq/backtrace/symbolizer.cpp
)
