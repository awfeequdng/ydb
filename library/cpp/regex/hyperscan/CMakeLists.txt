
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-regex-hyperscan)
target_link_libraries(cpp-regex-hyperscan PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-hyperscan
  libs-hyperscan-runtime_core2
  libs-hyperscan-runtime_corei7
  libs-hyperscan-runtime_avx2
  libs-hyperscan-runtime_avx512
)
target_sources(cpp-regex-hyperscan PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/regex/hyperscan/hyperscan.cpp
)
