
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-containers-bitseq)
target_link_libraries(cpp-containers-bitseq PUBLIC
  contrib-libs-cxxsupp
  yutil
  util-draft
  library-cpp-pop_count
)
target_sources(cpp-containers-bitseq PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/containers/bitseq/bitvector.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/containers/bitseq/readonly_bitvector.cpp
)
