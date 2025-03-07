cmake_minimum_required(VERSION 3.16)
project(dockwidgets LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT DEFINED INSTALL_EXAMPLESDIR)
    set(INSTALL_EXAMPLESDIR "examples")
endif()

set(INSTALL_EXAMPLEDIR "${INSTALL_EXAMPLESDIR}/widgets/mainwindows/dockwidgets")

find_package(Qt6
    REQUIRED COMPONENTS Core Gui Widgets
    OPTIONAL_COMPONENTS PrintSupport
)

qt_add_executable(dockwidgets
    main.cpp
    mainwindow.cpp mainwindow.h
)

set_target_properties(dockwidgets PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(dockwidgets PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Widgets
)

if (TARGET Qt::PrintSupport)
    target_link_libraries(dockwidgets PUBLIC Qt::PrintSupport)
endif()

# Resources:
set(dockwidgets_resource_files
    "images/new.png"
    "images/print.png"
    "images/save.png"
    "images/undo.png"
)

qt_add_resources(dockwidgets "dockwidgets"
    PREFIX
        "/"
    FILES
        ${dockwidgets_resource_files}
)

if(TARGET Qt::PrintSupport)
    target_link_libraries(dockwidgets PUBLIC
        Qt::PrintSupport
    )
endif()

install(TARGETS dockwidgets
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
