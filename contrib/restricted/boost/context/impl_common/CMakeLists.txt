
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(boost-context-impl_common)
target_compile_options(boost-context-impl_common PRIVATE
  -DBOOST_CONTEXT_SOURCE
  -Wno-everything
)
target_include_directories(boost-context-impl_common PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/context/include
)
target_link_libraries(boost-context-impl_common PUBLIC
  contrib-libs-cxxsupp
  restricted-boost-assert
  restricted-boost-config
  restricted-boost-mp11
  restricted-boost-pool
  restricted-boost-predef
  restricted-boost-smart_ptr
  restricted-boost-thread
)
target_sources(boost-context-impl_common PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/context/src/posix/stack_traits.cpp
)
