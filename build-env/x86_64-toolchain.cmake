# Native x86_64 build in the desktop container (Containerfile.desktop).
# Libraries Debian doesn't have (tg_owt, tde2e, tdesktop_rust, ada) are built by build-deps.sh into /src/deps-x86_64.
set(CMAKE_PREFIX_PATH /src/deps-x86_64/prefix)
set(QSB_EXECUTABLE /usr/lib/qt6/bin/qsb CACHE FILEPATH "")
