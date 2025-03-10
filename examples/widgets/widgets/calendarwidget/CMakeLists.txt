cmake_minimum_required(VERSION 3.16)
project(calendarwidget LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT DEFINED INSTALL_EXAMPLESDIR)
    set(INSTALL_EXAMPLESDIR "examples")
endif()

set(INSTALL_EXAMPLEDIR "${INSTALL_EXAMPLESDIR}/widgets/widgets/calendarwidget")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(calendarwidget
    main.cpp
    window.cpp window.h
)

set_target_properties(calendarwidget PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(calendarwidget PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Widgets
)

install(TARGETS calendarwidget
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
