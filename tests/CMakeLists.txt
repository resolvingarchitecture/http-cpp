add_executable(http_tests
    test_main.cpp
    test_url.cpp
    test_request.cpp
    test_response.cpp
    test_client.cpp
)
target_include_directories(http_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_link_libraries(http_tests PRIVATE http)

find_package(Threads REQUIRED)
target_link_libraries(http_tests PRIVATE Threads::Threads)

include(CTest)
add_test(NAME http_tests COMMAND http_tests)
