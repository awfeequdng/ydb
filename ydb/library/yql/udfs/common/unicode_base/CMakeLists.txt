
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(lib)

add_library(unicode_udf INTERFACE)
target_link_libraries(unicode_udf INTERFACE
  contrib-libs-cxxsupp
  yutil
  yql-public-udf
  public-udf-support
  common-unicode_base-lib
)

add_global_library_for(unicode_udf.global unicode_udf)
target_compile_options(unicode_udf.global PRIVATE
  -DUDF_ABI_VERSION_MAJOR=2
  -DUDF_ABI_VERSION_MINOR=9
  -DUDF_ABI_VERSION_PATCH=0
)
target_link_libraries(unicode_udf.global PUBLIC
  contrib-libs-cxxsupp
  yutil
  yql-public-udf
  public-udf-support
  common-unicode_base-lib
)
target_sources(unicode_udf.global PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/udfs/common/unicode_base/unicode_base.cpp
)
