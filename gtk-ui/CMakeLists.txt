# Contrail: the GTK4 + libadwaita interface. Its sources are compiled into Telegram Desktop's own
# executable target (the patch in patches/ adds this directory), so they can use the whole core.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CONTRAIL_GTK_DEPS REQUIRED IMPORTED_TARGET GLOBAL gtk4 libadwaita-1)

# The stylesheet is compiled in.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/style.css)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/style.css CONTRAIL_CSS)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/style_css.h
  CONTENT "#pragma once\ninline constexpr char kStyleCss[] = R\"contrail_css(@CONTRAIL_CSS@)contrail_css\";\n" @ONLY)

file(GLOB CONTRAIL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
target_sources(Telegram PRIVATE ${CONTRAIL_SRC})
target_include_directories(Telegram PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(Telegram PRIVATE CONTRAIL_GTK)
target_link_libraries(Telegram PRIVATE PkgConfig::CONTRAIL_GTK_DEPS)
# GTK and libadwaita versions of FuriOS 14: warn about anything newer. Telegram Desktop limits GLib to 2.56
# for its own code, the interface needs newer.
set_source_files_properties(${CONTRAIL_SRC} PROPERTIES
  COMPILE_OPTIONS "-UGLIB_VERSION_MIN_REQUIRED;-UGLIB_VERSION_MAX_ALLOWED;-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_84;-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_84;-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_12;-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_22;-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_6;-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_9;-Wno-missing-field-initializers;-Wno-unused-parameter")
