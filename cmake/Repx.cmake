add_library(pvd_repx src/repx.cpp)
add_library(pvd::repx ALIAS pvd_repx)
set_target_properties(pvd_repx PROPERTIES EXPORT_NAME repx VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR} CXX_EXTENSIONS OFF)
target_compile_features(pvd_repx PUBLIC cxx_std_17)
target_link_libraries(pvd_repx PUBLIC pvd::core PRIVATE pvd::render)
target_compile_definitions(pvd_repx PRIVATE PVD_REPX_BUILD)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(pvd_repx PUBLIC PVD_REPX_STATIC)
endif()
if(MSVC)
    target_compile_options(pvd_repx PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pvd_repx PRIVATE -Wall -Wextra -Wpedantic)
endif()
list(APPEND PVD_INSTALL_TARGETS pvd_repx)

if(BUILD_TESTING)
    add_executable(pvd_repx_tests tests/repx_tests.cpp)
    target_link_libraries(pvd_repx_tests PRIVATE pvd::repx pvd::render)
    if(MSVC)
        target_compile_options(pvd_repx_tests PRIVATE /utf-8)
    endif()
    add_test(NAME repx COMMAND pvd_repx_tests)
    set_tests_properties(repx PROPERTIES TIMEOUT 30)
endif()

if(PVD_BUILD_PHYSX34_VALIDATION)
    include(${CMAKE_CURRENT_LIST_DIR}/PhysX34.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/RepxValidation.cmake)
    pvd_add_physx34_repx_validate(pvd_physx34_repx_validate)
    if(BUILD_TESTING)
        add_test(NAME physx34_repx_generate COMMAND pvd_repx_tests
            ${PROJECT_BINARY_DIR}/fixtures/render_capture.pxd2 ${PROJECT_BINARY_DIR}/fixtures/repx)
        set_tests_properties(physx34_repx_generate PROPERTIES FIXTURES_REQUIRED physx34_render
            FIXTURES_SETUP physx34_repx TIMEOUT 30)
        foreach(suffix IN ITEMS "" _big_endian _selected _shared _attachment _synthetic)
            add_test(NAME physx34_repx${suffix} COMMAND pvd_physx34_repx_validate
                ${PROJECT_BINARY_DIR}/fixtures/repx${suffix}.repx
                ${PROJECT_BINARY_DIR}/fixtures/repx${suffix}_expected.csv)
            set_tests_properties(physx34_repx${suffix} PROPERTIES FIXTURES_REQUIRED physx34_repx TIMEOUT 30)
        endforeach()
        add_test(NAME physx34_repx_official_roundtrip COMMAND pvd_physx34_repx_validate
            ${PROJECT_BINARY_DIR}/fixtures/repx.repx ${PROJECT_BINARY_DIR}/fixtures/repx_expected.csv
            --oracle ${PROJECT_BINARY_DIR}/fixtures/repx_official.repx)
        set_tests_properties(physx34_repx_official_roundtrip PROPERTIES FIXTURES_REQUIRED physx34_repx TIMEOUT 30)
    endif()
endif()
