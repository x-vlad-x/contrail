# Installs Contrail as the "Contrail" component (included by gtk-ui/CMakeLists.txt):
#   cmake --install <build> --component Contrail --prefix /usr
#
#   bin/contrail                      launcher (works under any prefix)
#   bin/contrail-migrate-launcher     replaces a user launcher left by the SSH install (run by the user; only
#                                     under the stable application ID)
#   libexec/contrail/contrail         the program: the GTK interface with TDLib compiled in
#   share/contrail/icons/             the interface's own symbolic icons
#   share/applications, share/icons/hicolor, share/metainfo, share/doc/contrail
#
# The desktop entry, the AppStream file and the icon carry CONTRAIL_APP_ID: the test version (".Devel") gets its
# own name in the menu and leaves tg:// links to the stable Contrail. CONTRAIL_COMMAND is the command the desktop
# entry starts (the DEB of the test version links it as /usr/bin/contrail-td).
set(CONTRAIL_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(CONTRAIL_COMPONENT Contrail)
set(CONTRAIL_STABLE_ID io.github.x_vlad_x.Contrail)
set(CONTRAIL_COMMAND contrail CACHE STRING "The command the desktop entry runs")
set(CONTRAIL_INSTALL_FILES ${CMAKE_CURRENT_BINARY_DIR}/install)

# The packages carry their libraries in lib/ beside libexec/: an RPATH (not a RUNPATH, which would serve only the
# program's own dependencies, not the libraries they need in turn).
option(CONTRAIL_BUNDLE_RPATH "Look for libraries in ../../lib first, as the packages lay them out" OFF)
if (CONTRAIL_BUNDLE_RPATH)
  set_target_properties(contrail PROPERTIES INSTALL_RPATH "$ORIGIN/../../lib")
  target_link_options(contrail PRIVATE LINKER:--disable-new-dtags)
endif()

file(READ ${CONTRAIL_ROOT}/packaging/${CONTRAIL_STABLE_ID}.desktop _entry)
file(READ ${CONTRAIL_ROOT}/packaging/${CONTRAIL_STABLE_ID}.metainfo.xml _metainfo)
set(_icon ${CONTRAIL_ROOT}/packaging/${CONTRAIL_STABLE_ID}.svg)
string(REPLACE "Exec=contrail " "Exec=${CONTRAIL_COMMAND} " _entry "${_entry}")
if (NOT CONTRAIL_APP_ID STREQUAL CONTRAIL_STABLE_ID)
  string(REPLACE "${CONTRAIL_STABLE_ID}" "${CONTRAIL_APP_ID}" _entry "${_entry}")
  string(REPLACE "${CONTRAIL_STABLE_ID}" "${CONTRAIL_APP_ID}" _metainfo "${_metainfo}")
  string(REPLACE "Name=Contrail\n" "Name=${CONTRAIL_APP_NAME}\n" _entry "${_entry}")
  string(REPLACE "<name>Contrail</name>" "<name>${CONTRAIL_APP_NAME}</name>" _metainfo "${_metainfo}")
  string(REPLACE "<binary>contrail</binary>" "<binary>${CONTRAIL_COMMAND}</binary>" _metainfo "${_metainfo}")
  # tg:// links stay with the stable Contrail: the test version is installed next to it.
  string(REGEX REPLACE "\nMimeType=[^\n]*" "" _entry "${_entry}")
  set(_icon ${CONTRAIL_ROOT}/tools/contrail-td-icon.svg)
endif()
file(WRITE ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.desktop "${_entry}")
file(WRITE ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.metainfo.xml "${_metainfo}")
configure_file(${_icon} ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.svg COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONTRAIL_ROOT}/packaging/${CONTRAIL_STABLE_ID}.desktop
  ${CONTRAIL_ROOT}/packaging/${CONTRAIL_STABLE_ID}.metainfo.xml)

install(TARGETS contrail RUNTIME DESTINATION libexec/contrail COMPONENT ${CONTRAIL_COMPONENT})
install(PROGRAMS ${CONTRAIL_ROOT}/packaging/contrail DESTINATION bin COMPONENT ${CONTRAIL_COMPONENT})
if (CONTRAIL_APP_ID STREQUAL CONTRAIL_STABLE_ID)
  install(PROGRAMS ${CONTRAIL_ROOT}/packaging/contrail-migrate-launcher DESTINATION bin COMPONENT ${CONTRAIL_COMPONENT})
endif()
install(DIRECTORY ${CONTRAIL_ROOT}/gtk-ui/icons/ DESTINATION share/contrail/icons COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.desktop DESTINATION share/applications
  COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.metainfo.xml DESTINATION share/metainfo
  COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_INSTALL_FILES}/${CONTRAIL_APP_ID}.svg DESTINATION share/icons/hicolor/scalable/apps
  COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_ROOT}/LICENSE ${CONTRAIL_ROOT}/NOTICE ${CONTRAIL_ROOT}/packaging/LICENSES.md
  DESTINATION share/doc/contrail COMPONENT ${CONTRAIL_COMPONENT})
# TDLib is compiled in (Boost Software License 1.0), with its copy of SQLite (public domain).
install(FILES ${CONTRAIL_TD_DIR}/src/LICENSE_1_0.txt DESTINATION share/doc/contrail/tdlib COMPONENT ${CONTRAIL_COMPONENT})
