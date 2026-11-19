cmake_minimum_required(VERSION 3.21)
project(myrddin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
# (SVG pictures are read through Qt's image plugin at run time; linking the module only makes
# deployment tools carry that plugin along)
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS Svg)
qt_standard_project_setup(REQUIRES 6.5)

file(GLOB MYRDDIN_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
file(GLOB MYRDDIN_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
file(GLOB MYRDDIN_ASSETS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS assets/*)

# The module is in the resources at qrc:/qt/qml/Myrddin, which is on Qt's import path wherever the program
# is installed. The pages lie beside the module's qmldir there (not in a qml/ folder), so that each of them
# sees the module's types, the Theme singleton among them, without importing anything.
foreach(file ${MYRDDIN_QML})
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(myrddin ${MYRDDIN_SOURCES})
target_compile_definitions(myrddin PRIVATE MYRDDIN_VERSION="${PROJECT_VERSION}")
qt_add_qml_module(myrddin
    URI Myrddin
    VERSION 1.0
    QML_FILES ${MYRDDIN_QML}
    RESOURCES ${MYRDDIN_ASSETS}
)
target_link_libraries(myrddin PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
if(TARGET Qt6::Svg)
    target_link_libraries(myrddin PRIVATE Qt6::Svg)
endif()

include(GNUInstallDirs)
install(TARGETS myrddin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.cadwaladr.Myrddin.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/io.github.cadwaladr.Myrddin.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/io.github.cadwaladr.Myrddin.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
