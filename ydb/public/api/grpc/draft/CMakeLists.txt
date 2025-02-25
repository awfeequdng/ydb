
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(api-grpc-draft)
set_property(TARGET api-grpc-draft PROPERTY
  PROTOC_EXTRA_OUTS .grpc.pb.cc .grpc.pb.h
)
target_link_libraries(api-grpc-draft PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-grpc
  api-protos
  contrib-libs-protobuf
)
target_proto_messages(api-grpc-draft PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/dummy.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_clickhouse_internal_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_persqueue_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_datastreams_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_experimental_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_s3_internal_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_long_tx_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_logstore_v1.proto
  ${CMAKE_SOURCE_DIR}/ydb/public/api/grpc/draft/ydb_topic_tx_v1.proto
)
target_proto_addincls(api-grpc-draft
  ./
  ${CMAKE_SOURCE_DIR}/
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
)
target_proto_outs(api-grpc-draft
  --cpp_out=${CMAKE_BINARY_DIR}/
  --cpp_styleguide_out=${CMAKE_BINARY_DIR}/
)
target_proto_plugin(api-grpc-draft
  grpc_cpp
  grpc_cpp
)
