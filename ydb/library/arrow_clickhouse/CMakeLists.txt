
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(Columns)
add_subdirectory(Common)
add_subdirectory(DataStreams)
add_subdirectory(ut)

add_library(ydb-library-arrow_clickhouse)
target_include_directories(ydb-library-arrow_clickhouse PUBLIC
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/udfs/common/clickhouse/client/base
)
target_include_directories(ydb-library-arrow_clickhouse PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/arrow_clickhouse
)
target_link_libraries(ydb-library-arrow_clickhouse PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-apache-arrow
  library-arrow_clickhouse-Common
  library-arrow_clickhouse-Columns
  library-arrow_clickhouse-DataStreams
)
target_sources(ydb-library-arrow_clickhouse PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/arrow_clickhouse/AggregateFunctions/IAggregateFunction.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/arrow_clickhouse/Aggregator.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/udfs/common/clickhouse/client/base/common/mremap.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/udfs/common/clickhouse/client/base/common/getPageSize.cpp
)
