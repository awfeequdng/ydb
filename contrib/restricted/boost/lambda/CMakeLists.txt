
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-lambda INTERFACE)
target_include_directories(restricted-boost-lambda INTERFACE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/lambda/include
)
target_link_libraries(restricted-boost-lambda INTERFACE
  contrib-libs-cxxsupp
  yutil
  restricted-boost-bind
  restricted-boost-config
  restricted-boost-core
  restricted-boost-detail
  restricted-boost-iterator
  restricted-boost-mpl
  restricted-boost-preprocessor
  restricted-boost-tuple
  restricted-boost-type_traits
  restricted-boost-utility
)
