
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


find_package(OpenSSL REQUIRED)

add_library(restricted-aws-aws-c-cal)
target_compile_options(restricted-aws-aws-c-cal PRIVATE
  -Wno-everything
)
target_include_directories(restricted-aws-aws-c-cal PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/include
)
target_link_libraries(restricted-aws-aws-c-cal PUBLIC
  OpenSSL::OpenSSL
  restricted-aws-aws-c-common
)
target_sources(restricted-aws-aws-c-cal PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/cal.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/der.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/ecc.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/hash.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/hmac.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/unix/openssl_platform_init.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/unix/opensslcrypto_ecc.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/unix/opensslcrypto_hash.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/aws/aws-c-cal/source/unix/opensslcrypto_hmac.c
)
