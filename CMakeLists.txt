cmake_minimum_required(VERSION 3.24)
project(ImageHandler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QT_ROOT "D:/tools/3rdparty/Qt/6.8.2/msvc2022_64" CACHE PATH "Qt installation root")
set(VIPS_ROOT "D:/3rd/vips-dev-8.18" CACHE PATH "libvips installation root")
list(PREPEND CMAKE_PREFIX_PATH "${QT_ROOT}")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent Test)
option(IMAGE_HANDLER_ENABLE_VULKAN_JOB_PREVIEW "Build the Vulkan Job preview" ON)
if(IMAGE_HANDLER_ENABLE_VULKAN_JOB_PREVIEW)
    find_package(Vulkan QUIET)
    find_package(Qt6 6.8 QUIET COMPONENTS ShaderTools)
endif()
qt_standard_project_setup()

add_library(image_handler_core
    src/domain/Models.h
    src/domain/JobModels.h
    src/domain/PointCloudModels.h
    src/domain/MaskSliceModels.h
    src/domain/StorageSizeFormatter.h src/domain/StorageSizeFormatter.cpp
    src/domain/MergeInputSelector.h src/domain/MergeInputSelector.cpp
    src/imaging/VipsSupport.h src/imaging/VipsSupport.cpp
    src/imaging/BmpRegionLoader.h src/imaging/BmpRegionLoader.cpp
    src/imaging/ImageLoader.h src/imaging/ImageLoader.cpp
    src/imaging/MosaicBuilder.h src/imaging/MosaicBuilder.cpp
    src/imaging/PatternDetector.h src/imaging/PatternDetector.cpp
    src/imaging/ElementGrouping.h src/imaging/ElementGrouping.cpp
    src/imaging/JobPreviewBuilder.h src/imaging/JobPreviewBuilder.cpp
    src/imaging/JobLabelLod.h src/imaging/JobLabelLod.cpp
    src/imaging/JobPreviewMath.h src/imaging/JobPreviewMath.cpp
    src/imaging/JobSpatialIndex.h src/imaging/JobSpatialIndex.cpp
    src/imaging/PointCloudBuilder.h src/imaging/PointCloudBuilder.cpp
    src/imaging/PointCloudMath.h src/imaging/PointCloudMath.cpp
    src/imaging/MaskSliceGenerator.h src/imaging/MaskSliceGenerator.cpp
    src/imaging/LayeredMaskProcessor.h src/imaging/LayeredMaskProcessor.cpp
    src/imaging/MaskStackMeshBuilder.h src/imaging/MaskStackMeshBuilder.cpp
    src/io/DirectoryScanner.h src/io/DirectoryScanner.cpp
    src/io/BmpFormat.h
    src/io/BmpStreamWriter.h src/io/BmpStreamWriter.cpp
    src/io/PngWriter.h src/io/PngWriter.cpp
    src/io/MergedImageExporter.h src/io/MergedImageExporter.cpp
    src/io/MaskSliceExporter.h src/io/MaskSliceExporter.cpp
    src/io/ExportTypes.h
    src/io/LayeredPackageTypes.h
    src/io/LayeredPackageExporter.h src/io/LayeredPackageExporter.cpp
    src/io/ExportPreflight.h src/io/ExportPreflight.cpp
    src/io/SplitExporter.h src/io/SplitExporter.cpp
    src/io/JobFileLoader.h src/io/JobFileLoader.cpp
    src/io/JobPackageLoader.h src/io/JobPackageLoader.cpp
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
target_compile_definitions(image_handler_core PUBLIC
    QT_NO_KEYWORDS
    IMAGE_HANDLER_VERSION="${PROJECT_VERSION}"
)
if(MSVC)
    target_compile_options(image_handler_core PUBLIC /utf-8)
endif()

set(JOB_PREVIEW_HAS_VULKAN OFF)
if(IMAGE_HANDLER_ENABLE_VULKAN_JOB_PREVIEW AND Vulkan_FOUND AND TARGET Qt6::qsb)
    set(JOB_PREVIEW_HAS_VULKAN ON)
endif()

add_library(job_preview_ui STATIC
    src/ui/JobPreviewWidget.h src/ui/JobPreviewWidget.cpp
    src/ui/PointCloudWidget.h src/ui/PointCloudWidget.cpp
    src/ui/MaskStackWidget.h src/ui/MaskStackWidget.cpp
)
target_include_directories(job_preview_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(job_preview_ui PUBLIC image_handler_core Qt6::Widgets)
target_compile_definitions(job_preview_ui PUBLIC
    IMAGE_HANDLER_HAS_VULKAN_PREVIEW=$<BOOL:${JOB_PREVIEW_HAS_VULKAN}>)
if(JOB_PREVIEW_HAS_VULKAN)
    target_include_directories(job_preview_ui PRIVATE ${Vulkan_INCLUDE_DIRS})
    set(JOB_SHADER_OUTPUTS)
    foreach(JOB_SHADER IN ITEMS job_points.vert job_points.frag job_lines.vert job_lines.frag job_stack.vert job_stack.frag)
        set(JOB_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/${JOB_SHADER}")
        set(JOB_SHADER_PACK "${CMAKE_CURRENT_BINARY_DIR}/job_shaders/${JOB_SHADER}.qsb")
        set(JOB_SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/job_shaders/${JOB_SHADER}.spv")
        add_custom_command(OUTPUT "${JOB_SHADER_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/job_shaders"
            COMMAND $<TARGET_FILE:Qt6::qsb> -o "${JOB_SHADER_PACK}" "${JOB_SHADER_SOURCE}"
            COMMAND $<TARGET_FILE:Qt6::qsb> -x spirv,100 -o "${JOB_SHADER_SPV}" "${JOB_SHADER_PACK}"
            DEPENDS "${JOB_SHADER_SOURCE}" Qt6::qsb
            VERBATIM)
        set_source_files_properties("${JOB_SHADER_SPV}" PROPERTIES
            GENERATED TRUE QT_RESOURCE_ALIAS "${JOB_SHADER}.spv")
        list(APPEND JOB_SHADER_OUTPUTS "${JOB_SHADER_SPV}")
    endforeach()
    qt_add_resources(job_preview_ui job_preview_shaders
        PREFIX "/job_shaders" FILES ${JOB_SHADER_OUTPUTS})
    set(CLOUD_SHADER_OUTPUTS)
    foreach(CLOUD_SHADER IN ITEMS cloud_points.vert cloud_points.frag cloud_lines.vert cloud_lines.frag)
        set(CLOUD_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/${CLOUD_SHADER}")
        set(CLOUD_SHADER_PACK "${CMAKE_CURRENT_BINARY_DIR}/cloud_shaders/${CLOUD_SHADER}.qsb")
        set(CLOUD_SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/cloud_shaders/${CLOUD_SHADER}.spv")
        add_custom_command(OUTPUT "${CLOUD_SHADER_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/cloud_shaders"
            COMMAND $<TARGET_FILE:Qt6::qsb> -o "${CLOUD_SHADER_PACK}" "${CLOUD_SHADER_SOURCE}"
            COMMAND $<TARGET_FILE:Qt6::qsb> -x spirv,100 -o "${CLOUD_SHADER_SPV}" "${CLOUD_SHADER_PACK}"
            DEPENDS "${CLOUD_SHADER_SOURCE}" Qt6::qsb
            VERBATIM)
        set_source_files_properties("${CLOUD_SHADER_SPV}" PROPERTIES
            GENERATED TRUE QT_RESOURCE_ALIAS "${CLOUD_SHADER}.spv")
        list(APPEND CLOUD_SHADER_OUTPUTS "${CLOUD_SHADER_SPV}")
    endforeach()
    qt_add_resources(job_preview_ui point_cloud_shaders
        PREFIX "/point_cloud_shaders" FILES ${CLOUD_SHADER_OUTPUTS})
    set(MASK_STACK_SHADER_OUTPUTS)
    foreach(MASK_STACK_SHADER IN ITEMS mask_stack.vert mask_stack.frag mask_stack_lines.vert mask_stack_lines.frag)
        set(MASK_STACK_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/${MASK_STACK_SHADER}")
        set(MASK_STACK_SHADER_PACK "${CMAKE_CURRENT_BINARY_DIR}/mask_stack_shaders/${MASK_STACK_SHADER}.qsb")
        set(MASK_STACK_SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/mask_stack_shaders/${MASK_STACK_SHADER}.spv")
        add_custom_command(OUTPUT "${MASK_STACK_SHADER_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/mask_stack_shaders"
            COMMAND $<TARGET_FILE:Qt6::qsb> -o "${MASK_STACK_SHADER_PACK}" "${MASK_STACK_SHADER_SOURCE}"
            COMMAND $<TARGET_FILE:Qt6::qsb> -x spirv,100 -o "${MASK_STACK_SHADER_SPV}" "${MASK_STACK_SHADER_PACK}"
            DEPENDS "${MASK_STACK_SHADER_SOURCE}" Qt6::qsb
            VERBATIM)
        set_source_files_properties("${MASK_STACK_SHADER_SPV}" PROPERTIES
            GENERATED TRUE QT_RESOURCE_ALIAS "${MASK_STACK_SHADER}.spv")
        list(APPEND MASK_STACK_SHADER_OUTPUTS "${MASK_STACK_SHADER_SPV}")
    endforeach()
    qt_add_resources(job_preview_ui mask_stack_shaders
        PREFIX "/mask_stack_shaders" FILES ${MASK_STACK_SHADER_OUTPUTS})
else()
    message(WARNING "Vulkan SDK headers or Qt ShaderTools were not found; Job preview will use the disabled placeholder.")
endif()

qt_add_executable(ImageHandler WIN32
    src/app/main.cpp
    src/ui/LargeImageView.h src/ui/LargeImageView.cpp
    src/ui/ElementPointCloudWindow.h src/ui/ElementPointCloudWindow.cpp
    src/ui/MaskSliceWindow.h src/ui/MaskSliceWindow.cpp
    src/ui/LayeredExportDialog.h src/ui/LayeredExportDialog.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
)
target_include_directories(ImageHandler PRIVATE
    "${VIPS_ROOT}/include"
    "${VIPS_ROOT}/include/glib-2.0"
    "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(ImageHandler PRIVATE image_handler_core Qt6::Widgets)
target_link_libraries(ImageHandler PRIVATE job_preview_ui)

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

qt_add_executable(ImageHandlerPerformance tests/PerformanceBenchmark.cpp)
target_link_libraries(ImageHandlerPerformance PRIVATE image_handler_core)

qt_add_executable(JobDataTests tests/TestJob.cpp)
target_link_libraries(JobDataTests PRIVATE image_handler_core Qt6::Test)
add_test(NAME JobDataTests COMMAND JobDataTests)
set_property(TEST JobDataTests PROPERTY ENVIRONMENT_MODIFICATION
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
    src/ui/ElementPointCloudWindow.h src/ui/ElementPointCloudWindow.cpp
    src/ui/MaskSliceWindow.h src/ui/MaskSliceWindow.cpp
    src/ui/LayeredExportDialog.h src/ui/LayeredExportDialog.cpp
)
target_include_directories(MainWindowTests PRIVATE
    "${VIPS_ROOT}/include"
    "${VIPS_ROOT}/include/glib-2.0"
    "${VIPS_ROOT}/lib/glib-2.0/include"
)
target_link_libraries(MainWindowTests PRIVATE image_handler_core Qt6::Widgets Qt6::Test)
target_link_libraries(MainWindowTests PRIVATE job_preview_ui)
add_test(NAME MainWindowTests COMMAND MainWindowTests)
set_property(TEST MainWindowTests PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${QT_ROOT}/bin"
    "PATH=path_list_prepend:${VIPS_ROOT}/bin")
