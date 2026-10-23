# Contrail: the GTK4 + libadwaita interface. Its sources are compiled into Telegram Desktop's own
# executable target (the patch in patches/ adds this directory), so they can use the whole core.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CONTRAIL_GTK_DEPS REQUIRED IMPORTED_TARGET GLOBAL gtk4 libadwaita-1)

# The stylesheet is compiled in.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/style.css)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/style.css CONTRAIL_CSS)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/style_css.h
  CONTENT "#pragma once\ninline constexpr char kStyleCss[] = R\"contrail_css(@CONTRAIL_CSS@)contrail_css\";\n" @ONLY)

# Contrail's own version (VERSION, independent of the core's), for About and the packages.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION CONTRAIL_VERSION LIMIT_COUNT 1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/contrail_version.h
  CONTENT "#pragma once\ninline constexpr char kContrailVersion[] = \"@CONTRAIL_VERSION@\";\n" @ONLY)

file(GLOB CONTRAIL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
target_sources(Telegram PRIVATE ${CONTRAIL_SRC})
target_include_directories(Telegram PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(Telegram PRIVATE CONTRAIL_GTK)
target_link_libraries(Telegram PRIVATE PkgConfig::CONTRAIL_GTK_DEPS)
# Optional: an incoming call over the phone's lock screen. The library wraps libwayland-client's calls,
# so the executable itself lists it (libwayland-client comes later, through GTK).
pkg_check_modules(CONTRAIL_LAYER_SHELL IMPORTED_TARGET GLOBAL gtk4-layer-shell-0)
if (CONTRAIL_LAYER_SHELL_FOUND)
  target_compile_definitions(Telegram PRIVATE CONTRAIL_LAYER_SHELL)
  target_link_libraries(Telegram PRIVATE PkgConfig::CONTRAIL_LAYER_SHELL)
endif()
# Optional: maps for locations (picking a place, following a live location). Without it a location opens in
# the system's map application.
pkg_check_modules(CONTRAIL_SHUMATE IMPORTED_TARGET GLOBAL shumate-1.0)
if (CONTRAIL_SHUMATE_FOUND)
  target_compile_definitions(Telegram PRIVATE CONTRAIL_SHUMATE)
  target_link_libraries(Telegram PRIVATE PkgConfig::CONTRAIL_SHUMATE)
endif()
# GTK and libadwaita versions of FuriOS 14: warn about anything newer. Telegram Desktop limits GLib to 2.56
# for its own code, the interface needs newer.
set_source_files_properties(${CONTRAIL_SRC} PROPERTIES
  COMPILE_OPTIONS "-UGLIB_VERSION_MIN_REQUIRED;-UGLIB_VERSION_MAX_ALLOWED;-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_84;-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_84;-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_12;-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_22;-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_6;-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_9;-Wno-missing-field-initializers;-Wno-unused-parameter")

# The release packages carry their libraries in <prefix>/lib (tools/bundle.py). A DT_RPATH (not RUNPATH) on the
# executable also applies to the libraries it loads, so the bundled ones are used without editing them.
option(CONTRAIL_BUNDLE_RPATH "Look for libraries in ../../lib next to the installed program" OFF)
if (CONTRAIL_BUNDLE_RPATH)
  target_link_options(Telegram PRIVATE "LINKER:-rpath,$ORIGIN/../../lib" "LINKER:--disable-new-dtags")
  # No build-tree path next to it (shared libraries built by build-deps.sh would add one): editing it out later
  # would turn the RPATH into a RUNPATH, which the libraries the program loads don't use.
  set_target_properties(Telegram PROPERTIES SKIP_BUILD_RPATH ON)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../packaging/install.cmake)
