cmake_minimum_required(VERSION 3.16)
project(rasterwindow LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT DEFINED INSTALL_EXAMPLESDIR)
    set(INSTALL_EXAMPLESDIR "examples")
endif()

set(INSTALL_EXAMPLEDIR "${INSTALL_EXAMPLESDIR}/gui/rasterwindow")

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

qt_add_executable(rasterwindow
    main.cpp
    rasterwindow.cpp rasterwindow.h
)

set_target_properties(rasterwindow PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_include_directories(rasterwindow PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rasterwindow PUBLIC
    Qt::Core
    Qt::Gui
)

install(TARGETS rasterwindow
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
