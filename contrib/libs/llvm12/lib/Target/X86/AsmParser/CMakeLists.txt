
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(Target-X86-AsmParser)
target_compile_options(Target-X86-AsmParser PRIVATE
  -Wno-everything
)
target_include_directories(Target-X86-AsmParser PRIVATE
  ${CMAKE_BINARY_DIR}/contrib/libs/llvm12/lib/Target/X86
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/AsmParser
)
target_link_libraries(Target-X86-AsmParser PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  libs-llvm12-include
  llvm12-lib-MC
  lib-MC-MCParser
  llvm12-lib-Support
  Target-X86-MCTargetDesc
  Target-X86-TargetInfo
)
target_sources(Target-X86-AsmParser PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/AsmParser/X86AsmParser.cpp
)
