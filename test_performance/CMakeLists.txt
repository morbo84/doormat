#
# Doormat tests configuration
#

set(DOORMAT_PERFORMANCE_TEST_EXECUTABLE doormatperf)

set(DOORMAT_SOURCES_DIR ${CMAKE_SOURCE_DIR}/${PROJECT_SOURCES})
set(DOORMA_PERF_CLIENT_DIR client)

set(TEST_PERFORMANCE_SOURCES
# 	${DOORMAT_SOURCES}
	${CMAKE_SOURCE_DIR}/test/nodes/common.cpp
	${DOORMA_PERF_CLIENT_DIR}/perf_client.cpp
	connector_perf.cpp
	cache.cpp
	chain_perf.cpp)

add_executable(${DOORMAT_PERFORMANCE_TEST_EXECUTABLE} ${TEST_PERFORMANCE_SOURCES})

target_include_directories(
	${DOORMAT_PERFORMANCE_TEST_EXECUTABLE}
	PRIVATE ${DOORMAT_SOURCES_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/${PROJECT_PERFORMANCE_TESTS}
	PRIVATE ${NGHTTP2_INCLUDE_DIR}
	PRIVATE ${Boost_INCLUDE_DIRS}
	PRIVATE ${OPENSSL_INCLUDE_DIR}
	PRIVATE ${GOOGLETEST_INCLUDE_DIR}
	PRIVATE ${GMOCK_INCLUDE_DIRS}
	PRIVATE ${CYNNYPP_INCLUDE_DIRS}
	PRIVATE ${CITYHASH_INCLUDE_DIRS}
	PRIVATE ${ZLIB_INCLUDE_DIRS}
	PRIVATE ${SPDLOG_INCLUDE_DIR}
)

target_link_libraries(
	${DOORMAT_PERFORMANCE_TEST_EXECUTABLE}
	PRIVATE ${DOORMAT_COMMON_ARCHIVE_LIB}
	PRIVATE ${CMAKE_THREAD_LIBS_INIT}
	PRIVATE ${GOOGLETEST_LIBRARY}
	PRIVATE ${GOOGLETEST_MAIN_LIBRARY}
	PRIVATE ${GMOCK_BOTH_LIBRARIES}
	PRIVATE ${Boost_LIBRARIES}
	PRIVATE ${NGHTTP2_LIBRARY}
	PRIVATE ${NGHTTP2_ASIO_LIBRARY}
	PRIVATE ${OPENSSL_LIBRARIES}
	PRIVATE ${CYNNYPP_LIBRARIES}
	PRIVATE ${CITYHASH_LIBRARIES}
	PRIVATE ${ZLIB_LIBRARIES}
)
