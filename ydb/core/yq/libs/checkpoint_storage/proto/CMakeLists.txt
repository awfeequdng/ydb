
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(libs-checkpoint_storage-proto)
target_link_libraries(libs-checkpoint_storage-proto PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-graph_params-proto
  contrib-libs-protobuf
)
target_proto_messages(libs-checkpoint_storage-proto PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/yq/libs/checkpoint_storage/proto/graph_description.proto
)
target_proto_addincls(libs-checkpoint_storage-proto
  ./
  ${CMAKE_SOURCE_DIR}/
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
)
target_proto_outs(libs-checkpoint_storage-proto
  --cpp_out=${CMAKE_BINARY_DIR}/
  --cpp_styleguide_out=${CMAKE_BINARY_DIR}/
)
