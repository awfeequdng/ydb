
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(Target-X86-MCTargetDesc)
target_compile_options(Target-X86-MCTargetDesc PRIVATE
  -Wno-everything
)
target_include_directories(Target-X86-MCTargetDesc PRIVATE
  ${CMAKE_BINARY_DIR}/contrib/libs/llvm12/lib/Target/X86
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc
)
target_link_libraries(Target-X86-MCTargetDesc PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  libs-llvm12-include
  llvm12-lib-BinaryFormat
  llvm12-lib-MC
  lib-MC-MCDisassembler
  llvm12-lib-Support
  Target-X86-TargetInfo
)
target_sources(Target-X86-MCTargetDesc PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86ATTInstPrinter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86AsmBackend.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86ELFObjectWriter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86InstComments.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86InstPrinterCommon.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86IntelInstPrinter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86MCAsmInfo.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86MCCodeEmitter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86MCTargetDesc.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86MachObjectWriter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86ShuffleDecode.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86WinCOFFObjectWriter.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86WinCOFFStreamer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Target/X86/MCTargetDesc/X86WinCOFFTargetStreamer.cpp
)
