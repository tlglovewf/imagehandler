cmake_minimum_required(VERSION 3.24)
project(ImageHandler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QT_ROOT "D:/tools/3rdparty/Qt/6.8.2/msvc2022_64" CACHE PATH "Qt installation root")
set(VIPS_ROOT "D:/3rd/vips-dev-8.18" CACHE PATH "libvips installation root")
list(PREPEND CMAKE_PREFIX_PATH "${QT_ROOT}")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent Test)
qt_standard_project_setup()

add_library(image_handler_core
    src/domain/Models.h
    src/domain/MergeInputSelector.h src/domain/MergeInputSelector.cpp
    src/imaging/VipsSupport.h src/imaging/VipsSupport.cpp
    src/imaging/BmpRegionLoader.h src/imaging/BmpRegionLoader.cpp
    src/imaging/ImageLoader.h src/imaging/ImageLoader.cpp
    src/imaging/MosaicBuilder.h src/imaging/MosaicBuilder.cpp
    src/imaging/PatternDetector.h src/imaging/PatternDetector.cpp
    src/io/DirectoryScanner.h src/io/DirectoryScanner.cpp
    src/io/BmpStreamWriter.h src/io/BmpStreamWriter.cpp
    src/io/MergedImageExporter.h src/io/MergedImageExporter.cpp
    src/io/SplitExporter.h src/io/SplitExporter.cpp
    src/tasks/TaskManager.h src/tasks/TaskManager.cpp
)
target_include_directories(image_handler_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE
        "${VIPS_ROOT}/include"
        "${VIPS_ROOT}/include/glib-2.0"
        "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(image_handler_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent
    PRIVATE
        "${VIPS_ROOT}/lib/libvips.lib"
        "${VIPS_ROOT}/lib/libgobject-2.0.lib"
        "${VIPS_ROOT}/lib/libglib-2.0.lib"
)
target_compile_definitions(image_handler_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(image_handler_core PUBLIC QT_NO_KEYWORDS)
if(MSVC)
    target_compile_options(image_handler_core PUBLIC /utf-8)
endif()

qt_add_executable(ImageHandler WIN32
    src/app/main.cpp
    src/ui/LargeImageView.h src/ui/LargeImageView.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
)
target_include_directories(ImageHandler PRIVATE
    "${VIPS_ROOT}/include"
    "${VIPS_ROOT}/include/glib-2.0"
    "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(ImageHandler PRIVATE image_handler_core Qt6::Widgets)

if(WIN32)
    file(GLOB VIPS_RUNTIME_DLLS CONFIGURE_DEPENDS "${VIPS_ROOT}/bin/*.dll")
    foreach(VIPS_DLL IN LISTS VIPS_RUNTIME_DLLS)
        add_custom_command(TARGET ImageHandler POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VIPS_DLL}" "$<TARGET_FILE_DIR:ImageHandler>")
    endforeach()
    if(EXISTS "${VIPS_ROOT}/bin/vips-modules-8.18")
        add_custom_command(TARGET ImageHandler POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${VIPS_ROOT}/bin/vips-modules-8.18" "$<TARGET_FILE_DIR:ImageHandler>/vips-modules-8.18"
            COMMENT "Copying libvips loader modules")
    endif()
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_ROOT}/bin" REQUIRED)
    add_custom_command(TARGET ImageHandler POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:ImageHandler>"
        COMMENT "Deploying Qt runtime libraries")
endif()

enable_testing()
qt_add_executable(ImageHandlerTests
    tests/TestCore.cpp
)
target_link_libraries(ImageHandlerTests PRIVATE image_handler_core Qt6::Test)
add_test(NAME ImageHandlerTests COMMAND ImageHandlerTests)
set_property(TEST ImageHandlerTests PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${QT_ROOT}/bin"
    "PATH=path_list_prepend:${VIPS_ROOT}/bin")

qt_add_executable(LargeImageViewTests
    tests/TestLargeImageView.cpp
    src/ui/LargeImageView.h src/ui/LargeImageView.cpp
)
target_include_directories(LargeImageViewTests PRIVATE
    "${VIPS_ROOT}/include"
    "${VIPS_ROOT}/include/glib-2.0"
    "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(LargeImageViewTests PRIVATE image_handler_core Qt6::Widgets Qt6::Test)
add_test(NAME LargeImageViewTests COMMAND LargeImageViewTests)
set_property(TEST LargeImageViewTests PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${QT_ROOT}/bin"
    "PATH=path_list_prepend:${VIPS_ROOT}/bin")

qt_add_executable(MainWindowTests
    tests/TestMainWindow.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
    src/ui/LargeImageView.h src/ui/LargeImageView.cpp
)
target_include_directories(MainWindowTests PRIVATE
    "${VIPS_ROOT}/include"
    "${VIPS_ROOT}/include/glib-2.0"
    "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(MainWindowTests PRIVATE image_handler_core Qt6::Widgets Qt6::Test)
add_test(NAME MainWindowTests COMMAND MainWindowTests)
set_property(TEST MainWindowTests PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${QT_ROOT}/bin"
    "PATH=path_list_prepend:${VIPS_ROOT}/bin")
