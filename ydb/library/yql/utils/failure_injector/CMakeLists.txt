
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(ut)

add_library(yql-utils-failure_injector)
target_link_libraries(yql-utils-failure_injector PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-yql-utils
  yql-utils-log
)
target_sources(yql-utils-failure_injector PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/utils/failure_injector/failure_injector.cpp
)
