
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yq-libs-grpc)
set_property(TARGET yq-libs-grpc PROPERTY
  PROTOC_EXTRA_OUTS .grpc.pb.cc .grpc.pb.h
)
target_link_libraries(yq-libs-grpc PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-grpc
  api-protos
  yq-libs-protos
  contrib-libs-protobuf
)
target_proto_messages(yq-libs-grpc PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/yq/libs/grpc/fq_private_v1.proto
)
target_proto_addincls(yq-libs-grpc
  ./
  ${CMAKE_SOURCE_DIR}/
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
)
target_proto_outs(yq-libs-grpc
  --cpp_out=${CMAKE_BINARY_DIR}/
  --cpp_styleguide_out=${CMAKE_BINARY_DIR}/
)
target_proto_plugin(yq-libs-grpc
  grpc_cpp
  grpc_cpp
)
