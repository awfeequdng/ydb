
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-atomic)
target_compile_options(restricted-boost-atomic PRIVATE
  -DBOOST_ATOMIC_SOURCE
)
target_include_directories(restricted-boost-atomic PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/atomic/include
)
target_include_directories(restricted-boost-atomic PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/winapi/include
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/atomic/src
)
target_link_libraries(restricted-boost-atomic PUBLIC
  contrib-libs-cxxsupp
  yutil
  restricted-boost-align
  restricted-boost-assert
  restricted-boost-config
  restricted-boost-predef
  restricted-boost-preprocessor
  restricted-boost-type_traits
)
target_sources(restricted-boost-atomic PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/atomic/src/find_address_sse2.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/atomic/src/find_address_sse41.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/atomic/src/lock_pool.cpp
)
