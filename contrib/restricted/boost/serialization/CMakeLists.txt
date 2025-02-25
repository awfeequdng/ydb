
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-serialization)
target_compile_options(restricted-boost-serialization PRIVATE
  -Wno-everything
)
target_include_directories(restricted-boost-serialization PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/include
)
target_link_libraries(restricted-boost-serialization PUBLIC
  contrib-libs-cxxsupp
  restricted-boost-array
  restricted-boost-assert
  restricted-boost-config
  restricted-boost-core
  restricted-boost-detail
  restricted-boost-function
  restricted-boost-integer
  restricted-boost-io
  restricted-boost-iterator
  restricted-boost-move
  restricted-boost-mpl
  restricted-boost-optional
  restricted-boost-predef
  restricted-boost-preprocessor
  restricted-boost-smart_ptr
  restricted-boost-spirit
  restricted-boost-static_assert
  restricted-boost-type_traits
  restricted-boost-unordered
  restricted-boost-utility
  restricted-boost-variant
)
target_sources(restricted-boost-serialization PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/archive_exception.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_archive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_iserializer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_oserializer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_pointer_iserializer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_pointer_oserializer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_serializer_map.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_text_iprimitive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_text_oprimitive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_text_wiprimitive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_text_woprimitive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/basic_xml_archive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/binary_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/binary_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/binary_wiarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/binary_woarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/codecvt_null.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/extended_type_info.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/extended_type_info_no_rtti.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/extended_type_info_typeid.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_binary_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_binary_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_text_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_text_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_text_wiarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_text_woarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_xml_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_xml_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_xml_wiarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/polymorphic_xml_woarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/stl_port.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/text_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/text_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/text_wiarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/text_woarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/utf8_codecvt_facet.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/void_cast.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_archive_exception.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_grammar.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_iarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_oarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_wgrammar.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_wiarchive.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/serialization/src/xml_woarchive.cpp
)
