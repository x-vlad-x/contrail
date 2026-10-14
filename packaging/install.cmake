# Installs Contrail, and only Contrail, as the "Contrail" component (Telegram Desktop's own install rules are
# disabled by patches/01-gtk-hook.patch):
#   cmake --install <build> --component Contrail --prefix /usr
#
#   bin/contrail                      launcher (works under any prefix, also inside an AppImage)
#   bin/contrail-migrate-launcher     replaces a user launcher left by the SSH install (run by the user)
#   libexec/contrail/contrail         the program: Telegram Desktop's core with the GTK interface
#   share/contrail/icons/             the interface's own symbolic icons
#   share/applications, share/icons/hicolor, share/metainfo, share/doc/contrail
set(CONTRAIL_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(CONTRAIL_APP_ID io.github.x_vlad_x.Contrail)
set(CONTRAIL_COMPONENT Contrail)

install(PROGRAMS $<TARGET_FILE:Telegram> DESTINATION libexec/contrail RENAME contrail COMPONENT ${CONTRAIL_COMPONENT})
install(PROGRAMS ${CONTRAIL_ROOT}/packaging/contrail ${CONTRAIL_ROOT}/packaging/contrail-migrate-launcher
  DESTINATION bin COMPONENT ${CONTRAIL_COMPONENT})
install(DIRECTORY ${CONTRAIL_ROOT}/gtk-ui/icons/ DESTINATION share/contrail/icons COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_ROOT}/packaging/${CONTRAIL_APP_ID}.desktop
  DESTINATION share/applications COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_ROOT}/packaging/${CONTRAIL_APP_ID}.metainfo.xml
  DESTINATION share/metainfo COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_ROOT}/packaging/${CONTRAIL_APP_ID}.svg
  DESTINATION share/icons/hicolor/scalable/apps COMPONENT ${CONTRAIL_COMPONENT})
install(FILES ${CONTRAIL_ROOT}/LICENSE ${CONTRAIL_ROOT}/NOTICE ${CONTRAIL_ROOT}/packaging/LICENSES.md
  DESTINATION share/doc/contrail COMPONENT ${CONTRAIL_COMPONENT})
# Telegram Desktop's license and legal notes, and the license texts of the libraries compiled in from its
# source tree (ThirdParty and the desktop-app submodules).
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/LEGAL
  DESTINATION share/doc/contrail/upstream COMPONENT ${CONTRAIL_COMPONENT})
file(GLOB_RECURSE CONTRAIL_UPSTREAM_LICENSES RELATIVE ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Telegram/*LICENSE* ${CMAKE_SOURCE_DIR}/Telegram/*LICENCE*
  ${CMAKE_SOURCE_DIR}/Telegram/*COPYING* ${CMAKE_SOURCE_DIR}/Telegram/*NOTICE*
  ${CMAKE_SOURCE_DIR}/cmake/*LICENSE*)
foreach(_license IN LISTS CONTRAIL_UPSTREAM_LICENSES)
  if (NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/${_license} AND NOT _license MATCHES "(^|/)(\\.git|tests?|examples?)/|\\.(c|cc|cpp|h|hpp|m|mm|py)$")
    get_filename_component(_license_dir ${_license} DIRECTORY)
    install(FILES ${CMAKE_SOURCE_DIR}/${_license}
      DESTINATION share/doc/contrail/upstream/${_license_dir} COMPONENT ${CONTRAIL_COMPONENT})
  endif()
endforeach()
