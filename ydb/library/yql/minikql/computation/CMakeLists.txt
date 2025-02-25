
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yql-minikql-computation)
target_compile_options(yql-minikql-computation PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
  -Wno-everything
)
target_link_libraries(yql-minikql-computation PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-apache-arrow
  library-cpp-enumbitset
  library-cpp-packedtypes
  library-cpp-random_provider
  library-cpp-time_provider
  library-yql-minikql
  yql-minikql-arrow
  yql-public-udf
  library-yql-utils
  yql-minikql-codegen
  llvm12-lib-IR
  lib-ExecutionEngine-MCJIT
  llvm12-lib-Linker
  lib-Target-X86
  Target-X86-AsmParser
  lib-Transforms-IPO
  library-cpp-resource
)
target_sources(yql-minikql-computation PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_codegen.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_graph.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_graph_saveload.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_holders.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_impl.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_pack.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_computation_node_pack_impl.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_custom_list.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_validate.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_value_builder.cpp
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/presort.cpp
)

add_global_library_for(yql-minikql-computation.global yql-minikql-computation)
target_compile_options(yql-minikql-computation.global PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
  -Wno-everything
)
target_link_libraries(yql-minikql-computation.global PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-apache-arrow
  library-cpp-enumbitset
  library-cpp-packedtypes
  library-cpp-random_provider
  library-cpp-time_provider
  library-yql-minikql
  yql-minikql-arrow
  yql-public-udf
  library-yql-utils
  yql-minikql-codegen
  llvm12-lib-IR
  lib-ExecutionEngine-MCJIT
  llvm12-lib-Linker
  lib-Target-X86
  Target-X86-AsmParser
  lib-Transforms-IPO
  library-cpp-resource
)
target_sources(yql-minikql-computation.global PRIVATE
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/4dc9194f4b8472492eed05889d2e69e4.cpp
)
add_custom_command(
  OUTPUT
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_optimized.bc
  DEPENDS
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_merged.bc
  COMMAND
  ${LLVMOPT}
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_merged.bc
  -o
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_optimized.bc
  -O2
  -globalopt
  -globaldce
  -internalize
  -internalize-public-api-list=FetchNextItem#GetElement#GetVariantItem#NextListItem#NextDictItem#PackString#PackStringData#PackBool#PackByte#PackFloat#PackDouble#PackInt32#PackUInt32#PackInt64#PackUInt64#GetListIterator#GetDictIterator#GetOptionalValue
)
add_custom_command(
  OUTPUT
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_merged.bc
  DEPENDS
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack_bc.cpp.bc
  COMMAND
  ${LLVMLINK}
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack_bc.cpp.bc
  -o
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_merged.bc
)
llvm_compile_cxx(yql-minikql-computation.global
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/minikql/computation/mkql_pack_bc.cpp
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack_bc.cpp.bc
  ${CLANGPLUSPLUS}
  -Wno-unknown-warning-option
  -emit-llvm
)
resources(yql-minikql-computation.global
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/4dc9194f4b8472492eed05889d2e69e4.cpp
  INPUTS
  ${CMAKE_BINARY_DIR}/ydb/library/yql/minikql/computation/mkql_pack.bc_optimized.bc
  KEYS
  /llvm_bc/mkql_pack.bc
)
