
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(core-tx-scheme_cache)
target_compile_options(core-tx-scheme_cache PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(core-tx-scheme_cache PUBLIC
  contrib-libs-cxxsupp
  yutil
  ydb-core-base
  ydb-core-protos
  ydb-core-scheme
  ydb-library-aclib
  tools-enum_parser-enum_serialization_runtime
)
target_sources(core-tx-scheme_cache PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/scheme_cache/scheme_cache.cpp
)
generate_enum_serilization(core-tx-scheme_cache
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/scheme_cache/scheme_cache.h
  INCLUDE_HEADERS
  ydb/core/tx/scheme_cache/scheme_cache.h
)
