
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(greedy_dict)

add_library(library-cpp-codecs)
target_link_libraries(library-cpp-codecs PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-zstd
  library-cpp-bit_io
  library-cpp-blockcodecs
  cpp-codecs-greedy_dict
  library-cpp-comptable
  cpp-containers-comptrie
  cpp-deprecated-accessors
  library-cpp-packers
  cpp-string_utils-relaxed_escaper
)
target_sources(library-cpp-codecs PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/tls_cache.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/codecs.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/codecs_registry.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/comptable_codec.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/delta_codec.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/float_huffman.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/huffman_codec.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/pfor_codec.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/solar_codec.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/codecs/zstd_dict_codec.cpp
)
