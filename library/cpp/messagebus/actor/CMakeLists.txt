
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(messagebus_actor)
target_link_libraries(messagebus_actor PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-deprecated-atomic
)
target_sources(messagebus_actor PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/messagebus/actor/executor.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/messagebus/actor/thread_extra.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/messagebus/actor/what_thread_does.cpp
)
