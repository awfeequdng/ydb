
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-ExecutionEngine-MCJIT)
target_compile_options(lib-ExecutionEngine-MCJIT PRIVATE
  -Wno-everything
)
target_include_directories(lib-ExecutionEngine-MCJIT PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/ExecutionEngine/MCJIT
)
target_link_libraries(lib-ExecutionEngine-MCJIT PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  libs-llvm12-include
  llvm12-lib-ExecutionEngine
  lib-ExecutionEngine-RuntimeDyld
  llvm12-lib-IR
  llvm12-lib-Object
  llvm12-lib-Support
  llvm12-lib-Target
)
target_sources(lib-ExecutionEngine-MCJIT PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/ExecutionEngine/MCJIT/MCJIT.cpp
)
