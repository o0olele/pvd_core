add_library(pvd_viewer_model STATIC model.cpp)
target_include_directories(pvd_viewer_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pvd_viewer_model PUBLIC pvd::render)
target_compile_features(pvd_viewer_model PUBLIC cxx_std_17)

set(IMGUI_ROOT ${PROJECT_SOURCE_DIR}/third_party/imgui)
add_library(pvd_imgui STATIC
    ${IMGUI_ROOT}/imgui.cpp ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_win32.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp)
target_include_directories(pvd_imgui PUBLIC ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)
target_compile_definitions(pvd_imgui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pvd_imgui PRIVATE d3d11 d3dcompiler dwmapi)

add_executable(pvd_viewer WIN32 main.cpp renderer.cpp panels.cpp)
target_link_libraries(pvd_viewer PRIVATE pvd_viewer_model pvd_imgui
    d3d11 dxgi d3dcompiler windowscodecs ole32 comdlg32 shell32 dwmapi)
target_compile_definitions(pvd_viewer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
function(pvd_viewer_copy_runtime target)
    if(BUILD_SHARED_LIBS)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pvd_core> $<TARGET_FILE:pvd_render> $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()
pvd_viewer_copy_runtime(pvd_viewer)
install(TARGETS pvd_viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${IMGUI_ROOT}/LICENSE.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/pvd_viewer RENAME Dear-ImGui-LICENSE.txt)
if(MSVC)
    foreach(target pvd_viewer_model pvd_viewer)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(pvd_viewer_model_tests ${PROJECT_SOURCE_DIR}/tests/viewer_model_tests.cpp)
    target_link_libraries(pvd_viewer_model_tests PRIVATE pvd_viewer_model)
    pvd_viewer_copy_runtime(pvd_viewer_model_tests)
    add_test(NAME viewer_model COMMAND pvd_viewer_model_tests)
    set_tests_properties(viewer_model PROPERTIES TIMEOUT 30)
    if(PVD_BUILD_PHYSX34_VALIDATION)
        add_test(NAME viewer_physx34 COMMAND pvd_viewer_model_tests
            ${PROJECT_BINARY_DIR}/fixtures/render_capture.pxd2)
        set_tests_properties(viewer_physx34 PROPERTIES FIXTURES_REQUIRED physx34_render TIMEOUT 30)
        add_test(NAME viewer_gpu COMMAND pvd_viewer --smoke
            ${PROJECT_BINARY_DIR}/fixtures/render_capture.pxd2 --warp --hidden
            --report ${PROJECT_BINARY_DIR}/fixtures/viewer-smoke.json
            --screenshot ${PROJECT_BINARY_DIR}/fixtures/viewer-smoke.png)
        set_tests_properties(viewer_gpu PROPERTIES FIXTURES_REQUIRED physx34_render TIMEOUT 45)
    endif()
endif()
