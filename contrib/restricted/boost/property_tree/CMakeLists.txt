
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-property_tree INTERFACE)
target_include_directories(restricted-boost-property_tree INTERFACE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/property_tree/include
)
target_link_libraries(restricted-boost-property_tree INTERFACE
  contrib-libs-cxxsupp
  yutil
  restricted-boost-any
  restricted-boost-assert
  restricted-boost-bind
  restricted-boost-config
  restricted-boost-core
  restricted-boost-format
  restricted-boost-iterator
  restricted-boost-mpl
  restricted-boost-multi_index
  restricted-boost-optional
  restricted-boost-range
  restricted-boost-serialization
  restricted-boost-static_assert
  restricted-boost-throw_exception
  restricted-boost-type_traits
)
