
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-multi_index INTERFACE)
target_include_directories(restricted-boost-multi_index INTERFACE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/multi_index/include
)
target_link_libraries(restricted-boost-multi_index INTERFACE
  contrib-libs-cxxsupp
  yutil
  restricted-boost-assert
  restricted-boost-bind
  restricted-boost-config
  restricted-boost-container_hash
  restricted-boost-core
  restricted-boost-detail
  restricted-boost-foreach
  restricted-boost-integer
  restricted-boost-iterator
  restricted-boost-move
  restricted-boost-mpl
  restricted-boost-preprocessor
  restricted-boost-serialization
  restricted-boost-smart_ptr
  restricted-boost-static_assert
  restricted-boost-throw_exception
  restricted-boost-tuple
  restricted-boost-type_traits
  restricted-boost-utility
)
