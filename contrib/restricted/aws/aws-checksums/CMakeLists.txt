
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-aws-aws-checksums)
target_compile_options(restricted-aws-aws-checksums PRIVATE
  -DAWS_CHECKSUMS_USE_IMPORT_EXPORT
  -DAWS_COMMON_USE_IMPORT_EXPORT
  -DHAVE_SYSCONF
  -Wno-everything
)
target_include_directories(restricted-aws-aws-checksums PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-checksums/include
)
target_link_libraries(restricted-aws-aws-checksums PUBLIC
  restricted-aws-aws-c-common
)
target_sources(restricted-aws-aws-checksums PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-checksums/source/crc.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-checksums/source/crc_sw.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-checksums/source/intel/asm/crc32c_sse42_asm.c
)
