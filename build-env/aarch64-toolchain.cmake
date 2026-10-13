# Debian multiarch cross toolchain (x86_64 -> aarch64).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(PKG_CONFIG_EXECUTABLE /usr/bin/aarch64-linux-gnu-pkg-config CACHE FILEPATH "")
# Libraries Debian doesn't have (tg_owt, tde2e, tdesktop_rust) are built by build-deps.sh into /src/deps-aarch64.
set(CMAKE_PREFIX_PATH /src/deps-aarch64/prefix /usr/lib/aarch64-linux-gnu/cmake)
set(QT_HOST_PATH /usr CACHE PATH "")
set(QT_HOST_PATH_CMAKE_DIR /usr/lib/x86_64-linux-gnu/cmake CACHE PATH "")
# Generators built for aarch64 during the build (codegen, cppgir, ...) run through qemu-user.
set(CMAKE_CROSSCOMPILING_EMULATOR /usr/bin/qemu-aarch64 CACHE FILEPATH "")
# Programs (moc, protoc, wayland-scanner, qsb) come from the host, libraries/headers for the target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(QSB_EXECUTABLE /usr/lib/qt6/bin/qsb CACHE FILEPATH "")
set(DESKTOP_APP_GIRSCANNER /src/build-env/g-ir-scanner-cross CACHE FILEPATH "")
