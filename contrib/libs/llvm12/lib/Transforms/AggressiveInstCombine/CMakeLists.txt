
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-Transforms-AggressiveInstCombine)
target_compile_options(lib-Transforms-AggressiveInstCombine PRIVATE
  -Wno-everything
)
target_include_directories(lib-Transforms-AggressiveInstCombine PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Transforms/AggressiveInstCombine
)
target_link_libraries(lib-Transforms-AggressiveInstCombine PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  libs-llvm12-include
  llvm12-lib-Analysis
  llvm12-lib-IR
  llvm12-lib-Support
  lib-Transforms-Utils
)
target_sources(lib-Transforms-AggressiveInstCombine PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Transforms/AggressiveInstCombine/AggressiveInstCombine.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp
)
