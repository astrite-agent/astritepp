add_executable(astrite_tests
    main.cpp
    smoke_test.cpp
    value_test.cpp
    ir_test.cpp
    blackboard_test.cpp
    eval_test.cpp
    authority_test.cpp
    sse_test.cpp
    client_test.cpp
    runtime_test.cpp
    transcript_test.cpp
    sandbox_test.cpp
    trust_test.cpp
    fs_helper_test.cpp
    sha256_test.cpp
    unified_diff_test.cpp
    trust_store_test.cpp
    range_test.cpp
    task_test.cpp
    components_test.cpp
    control_plane_test.cpp
    tui_logic_test.cpp
    markdown_test.cpp
    tui_render_test.cpp
)
target_link_libraries(astrite_tests PRIVATE doctest astrite_core astrite_llm astrite_runtime
    astrite_sandbox ftxui::dom ftxui::screen)
target_include_directories(astrite_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
add_test(NAME astrite_tests COMMAND astrite_tests)
