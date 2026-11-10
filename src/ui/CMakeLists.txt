# UI layer. Built separately from the Qt-free engine libraries:
#   tdm_ui_core     model, formatting, EngineBridge, InstanceGuard (Qt6::Core + Network)
#   tdm_ui_widgets  windows, dialogs, delegates        (Qt6::Widgets)
#   TeslaDM         the application
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

add_library(tdm_ui_core STATIC
    format.cpp format.hpp
    download_model.cpp download_model.hpp
    engine_bridge.cpp engine_bridge.hpp
    instance_guard.cpp instance_guard.hpp)
target_link_libraries(tdm_ui_core PUBLIC tdm_options tdm_engine Qt6::Core Qt6::Network)

add_library(tdm_ui_widgets STATIC
    widgets.cpp widgets.hpp
    dialogs.cpp dialogs.hpp
    main_window.cpp main_window.hpp)
target_link_libraries(tdm_ui_widgets PUBLIC tdm_ui_core Qt6::Widgets)

qt_add_executable(tdm WIN32 main.cpp)
set_target_properties(tdm PROPERTIES OUTPUT_NAME "TeslaDM")
target_link_libraries(tdm PRIVATE tdm_options tdm_ui_widgets)

# Copy the Qt runtime next to the executables so they run from the build tree.
find_program(TDM_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
set(TDM_QT_PLUGIN_DIR "${Qt6_DIR}/../../../plugins" CACHE PATH "Qt plugin directory (offscreen platform for UI tests)")
function(tdm_deploy_qt target)
    if(TDM_WINDEPLOYQT)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${TDM_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --no-compiler-runtime --verbose 0 "$<TARGET_FILE:${target}>"
            COMMENT "windeployqt ${target}")
    endif()
endfunction()
tdm_deploy_qt(tdm)

# Install: the app, the DLLs it imports, and Qt plugins (platform, styles, TLS)
# deployed by windeployqt into the install tree.
install(TARGETS tdm RUNTIME DESTINATION bin)
install(FILES $<FILTER:$<TARGET_RUNTIME_DLLS:tdm>,EXCLUDE,[Ii][Cc][Uu]> DESTINATION bin)  # never ship Windows' ICU
# A Widgets app renders with the raster engine: skip the ~40 MB of software
# OpenGL and shader-compiler fallbacks that windeployqt adds by default.
qt_generate_deploy_app_script(TARGET tdm OUTPUT_SCRIPT TDM_QT_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR NO_TRANSLATIONS NO_COMPILER_RUNTIME
    DEPLOY_TOOL_OPTIONS --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler)
install(SCRIPT "${TDM_QT_DEPLOY_SCRIPT}")
# windeployqt copies Windows' own icuuc.dll (a system component since Windows 10
# 1903, byte-identical to System32's). Never redistribute OS files.
install(CODE "file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/icuuc.dll\")")
