
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-containers-absl_flat_hash)
target_include_directories(cpp-containers-absl_flat_hash PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/abseil-cpp
)
target_link_libraries(cpp-containers-absl_flat_hash PUBLIC
  contrib-libs-cxxsupp
  yutil
  abseil-cpp-absl-container
)
target_sources(cpp-containers-absl_flat_hash PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/containers/absl_flat_hash/flat_hash_map.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/containers/absl_flat_hash/flat_hash_set.cpp
)
