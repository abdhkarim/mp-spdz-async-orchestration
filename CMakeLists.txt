cmake_minimum_required(VERSION 3.20)

# Nom du projet
project(mp_spdz_async_orchestration LANGUAGES CXX)

# On force un standard moderne (C++20 recommandé)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option pratique : activer/désactiver les tests
option(BUILD_TESTS "Build unit tests" ON)

# ============================================================
# Dépendances (version simple)
# ============================================================

include(FetchContent)

# --- nlohmann/json (header-only) ---
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# --- cpp-httplib (header-only) : pratique pour un serveur/clients HTTP ---
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# --- libsodium (signatures/hash) ---
# On préfère find_package() si possible.
# Sur macOS : brew install libsodium
# Puis CMake devrait le trouver via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

message(STATUS "libsodium include dirs: ${SODIUM_INCLUDE_DIRS}")
message(STATUS "libsodium library dirs: ${SODIUM_LIBRARY_DIRS}")
message(STATUS "libsodium libraries: ${SODIUM_LIBRARIES}")

# ============================================================
# Sous-projets
# ============================================================

add_subdirectory(common)
add_subdirectory(consensus)
add_subdirectory(node)
add_subdirectory(spdz_bridge)

if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()