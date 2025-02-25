
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-actors-core)
target_link_libraries(cpp-actors-core PUBLIC
  contrib-libs-cxxsupp
  yutil
  tools-enum_parser-enum_serialization_runtime
  cpp-actors-memory_log
  cpp-actors-prof
  cpp-actors-protos
  cpp-actors-util
  library-cpp-execprofile
  cpp-json-writer
  library-cpp-logger
  library-cpp-lwtrace
  cpp-monlib-dynamic_counters
  library-cpp-svnversion
  cpp-threading-future
)
target_sources(cpp-actors-core PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/actor_coroutine.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/actor.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/actorid.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/actorsystem.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/ask.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/balancer.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/buffer.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/callstack.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/cpu_manager.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/event.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/event_pb.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/events_undelivered.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/executor_pool_base.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/executor_pool_basic.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/executor_pool_io.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/executor_pool_united.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/executor_thread.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/interconnect.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/io_dispatcher.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/log.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/log_settings.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/log_buffer.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/mailbox.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/monotonic.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/monotonic_provider.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/worker_context.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/probes.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/process_stats.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/scheduler_actor.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/scheduler_basic.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/scheduler_cookie.cpp
)
generate_enum_serilization(cpp-actors-core
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/defs.h
  INCLUDE_HEADERS
  library/cpp/actors/core/defs.h
)
generate_enum_serilization(cpp-actors-core
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/core/actor.h
  INCLUDE_HEADERS
  library/cpp/actors/core/actor.h
)
