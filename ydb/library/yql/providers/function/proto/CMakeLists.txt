
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-function-proto)
target_link_libraries(providers-function-proto PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-protobuf
)
target_proto_messages(providers-function-proto PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/function/proto/dq_function.proto
)
target_proto_addincls(providers-function-proto
  ./
  ${CMAKE_SOURCE_DIR}/
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
)
target_proto_outs(providers-function-proto
  --cpp_out=${CMAKE_BINARY_DIR}/
  --cpp_styleguide_out=${CMAKE_BINARY_DIR}/
)
