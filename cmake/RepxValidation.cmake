# A separate /MTd executable matches the supplied Extensions Debug archive.
# It only exchanges XML/CSV with the /MDd application, never C++ CRT objects.
function(pvd_add_physx34_repx_validate target_name)
    pvd_add_physx34_capture(${target_name}
        "${_PVD_PHYSX34_SOURCE_ROOT}/examples/physx34_repx_validate.cpp")
    set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
    target_compile_options(${target_name} PRIVATE /utf-8)
    set(_extensions "${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib")
    set(_cooking_lib "${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3CookingDEBUG_x64.lib")
    set(_cooking_dll "${PHYSX34_ROOT}/PhysX_3.4/Bin/vc15win64/PhysX3CookingDEBUG_x64.dll")
    foreach(_file IN ITEMS "${_extensions}" "${_cooking_lib}" "${_cooking_dll}")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "Missing RepX validation dependency: ${_file}")
        endif()
    endforeach()
    target_link_libraries(${target_name} PRIVATE "${_extensions}" "${_cooking_lib}")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cooking_dll}" "$<TARGET_FILE_DIR:${target_name}>"
        VERBATIM)
endfunction()
