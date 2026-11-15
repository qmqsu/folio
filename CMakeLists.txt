cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "0.8.38")
include("${CMAKE_CURRENT_LIST_DIR}/tools/folio_release_version.cmake")
folio_release_sequence("${PROJECT_VER}" FOLIO_RELEASE_SEQUENCE)
file(WRITE "${CMAKE_BINARY_DIR}/folio-release-version.json"
    "{\"version\":\"${PROJECT_VER}\",\"sequence\":${FOLIO_RELEASE_SEQUENCE}}\n")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(Folio)
