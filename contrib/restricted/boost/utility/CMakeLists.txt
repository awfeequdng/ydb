
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-utility INTERFACE)
target_include_directories(restricted-boost-utility INTERFACE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/utility/include
)
target_link_libraries(restricted-boost-utility INTERFACE
  contrib-libs-cxxsupp
  yutil
  restricted-boost-config
  restricted-boost-core
  restricted-boost-preprocessor
  restricted-boost-static_assert
  restricted-boost-throw_exception
  restricted-boost-type_traits
)
