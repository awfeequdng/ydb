
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-http-simple)
target_link_libraries(cpp-http-simple PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-http-io
  cpp-openssl-io
  cpp-string_utils-url
  library-cpp-uri
)
target_sources(cpp-http-simple PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/http/simple/http_client.cpp
)
