
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(ut)

add_library(core-kesus-proxy)
target_link_libraries(core-kesus-proxy PUBLIC
  contrib-libs-cxxsupp
  yutil
  ydb-core-base
  ydb-core-erasure
  core-kesus-tablet
  ydb-core-scheme
  core-tx-scheme_cache
  api-protos
)
target_sources(core-kesus-proxy PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/kesus/proxy/events.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kesus/proxy/proxy.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/kesus/proxy/proxy_actor.cpp
)
