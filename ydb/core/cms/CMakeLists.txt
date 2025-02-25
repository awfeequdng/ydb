
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


add_subdirectory(console)
add_subdirectory(ut)
add_subdirectory(ut_sentinel)

add_library(ydb-core-cms)
target_link_libraries(ydb-core-cms PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-resource
  cpp-actors-core
  ydb-core-actorlib_impl
  ydb-core-base
  ydb-core-blobstorage
  core-blobstorage-base
  core-blobstorage-crypto
  core-engine-minikql
  ydb-core-mind
  core-mind-bscontroller
  ydb-core-node_whiteboard
  ydb-core-protos
  core-protos-out
  ydb-core-tablet_flat
  core-tx-datashard
  ydb-library-aclib
  tools-enum_parser-enum_serialization_runtime
)
target_sources(ydb-core-cms PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/audit_log.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cluster_info.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_get_log_tail.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_init_scheme.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_load_state.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_log_and_send.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_log_cleanup.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_process_notification.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_reject_notification.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_remove_expired_notifications.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_remove_permissions.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_remove_request.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_remove_walle_task.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_store_permissions.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_store_walle_task.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_update_config.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/cms_tx_update_downtimes.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/downtime.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/erasure_checkers.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/http.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/info_collector.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/logger.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/sentinel.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/services.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/walle_api_handler.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/walle_check_task_adapter.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/walle_create_task_adapter.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/walle_list_tasks_adapter.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/walle_remove_task_adapter.cpp
)
generate_enum_serilization(ydb-core-cms
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/services.h
  INCLUDE_HEADERS
  ydb/core/cms/services.h
)

add_global_library_for(ydb-core-cms.global ydb-core-cms)
target_link_libraries(ydb-core-cms.global PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-resource
  cpp-actors-core
  ydb-core-actorlib_impl
  ydb-core-base
  ydb-core-blobstorage
  core-blobstorage-base
  core-blobstorage-crypto
  core-engine-minikql
  ydb-core-mind
  core-mind-bscontroller
  ydb-core-node_whiteboard
  ydb-core-protos
  core-protos-out
  ydb-core-tablet_flat
  core-tx-datashard
  ydb-library-aclib
  tools-enum_parser-enum_serialization_runtime
)
target_sources(ydb-core-cms.global PRIVATE
  ${CMAKE_BINARY_DIR}/ydb/core/cms/348ccbcf47529a3467f7e0dad6f7777d.cpp
)
resources(ydb-core-cms.global
  ${CMAKE_BINARY_DIR}/ydb/core/cms/348ccbcf47529a3467f7e0dad6f7777d.cpp
  INPUTS
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/index.html
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/cms.css
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/cms.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/cms_log.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/common.css
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/common.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/configs.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/config_forms.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard.css
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_hist.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_info.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_op.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_ops_list.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_rs.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/datashard_slow_ops.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/enums.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/ext/bootstrap.min.css
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/ext/bootstrap.bundle.min.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/ext/theme.blue.css
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/proto_types.js
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/res/edit.png
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/res/help.png
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/res/remove.png
  ${CMAKE_SOURCE_DIR}/ydb/core/cms/ui/validators.js
  KEYS
  cms/ui/index.html
  cms/ui/cms.css
  cms/ui/cms.js
  cms/ui/cms_log.js
  cms/ui/common.css
  cms/ui/common.js
  cms/ui/configs.js
  cms/ui/config_forms.js
  cms/ui/datashard.css
  cms/ui/datashard.js
  cms/ui/datashard_hist.js
  cms/ui/datashard_info.js
  cms/ui/datashard_op.js
  cms/ui/datashard_ops_list.js
  cms/ui/datashard_rs.js
  cms/ui/datashard_slow_ops.js
  cms/ui/enums.js
  cms/ui/ext/bootstrap.min.css
  cms/ui/ext/bootstrap.bundle.min.js
  cms/ui/ext/theme.blue.css
  cms/ui/proto_types.js
  cms/ui/res/edit.png
  cms/ui/res/help.png
  cms/ui/res/remove.png
  cms/ui/validators.js
)
