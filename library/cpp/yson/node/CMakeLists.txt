
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-yson-node)
target_link_libraries(cpp-yson-node PUBLIC
  contrib-libs-cxxsupp
  yutil
  tools-enum_parser-enum_serialization_runtime
  library-cpp-yson
  cpp-yson-json
)
target_sources(cpp-yson-node PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/node.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/node_io.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/node_builder.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/node_visitor.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/serialize.cpp
)
generate_enum_serilization(cpp-yson-node
  ${CMAKE_SOURCE_DIR}/library/cpp/yson/node/node.h
  INCLUDE_HEADERS
  library/cpp/yson/node/node.h
)
