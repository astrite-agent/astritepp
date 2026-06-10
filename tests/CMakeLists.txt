add_executable(astrite_tests
    main.cpp
    smoke_test.cpp
)
target_link_libraries(astrite_tests PRIVATE doctest third_party)
target_include_directories(astrite_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME astrite_tests COMMAND astrite_tests)
