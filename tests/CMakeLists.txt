add_executable(astrite_tests
    main.cpp
    smoke_test.cpp
    value_test.cpp
    ir_test.cpp
    blackboard_test.cpp
    eval_test.cpp
)
target_link_libraries(astrite_tests PRIVATE doctest astrite_core)
target_include_directories(astrite_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME astrite_tests COMMAND astrite_tests)
