
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-DebugInfo-Symbolize)
target_compile_options(lib-DebugInfo-Symbolize PRIVATE
  -Wno-everything
)
target_include_directories(lib-DebugInfo-Symbolize PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/DebugInfo/Symbolize
)
target_link_libraries(lib-DebugInfo-Symbolize PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  lib-DebugInfo-DWARF
  lib-DebugInfo-PDB
  llvm12-lib-Demangle
  llvm12-lib-Object
  llvm12-lib-Support
)
target_sources(lib-DebugInfo-Symbolize PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/DebugInfo/Symbolize/DIPrinter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/DebugInfo/Symbolize/SymbolizableObjectFile.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/DebugInfo/Symbolize/Symbolize.cpp
)
