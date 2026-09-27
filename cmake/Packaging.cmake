# cmake/Packaging.cmake — install layout + CPack packages of the editor and player (ADR-0013).
#
#   cmake --install <build> --prefix <dir>       stage an installed Aether in <dir>
#   cpack --config <build>/CPackConfig.cmake     build the packages (see scripts/package.ps1)
#
# Installed layout (the engine root is found as the executable's ancestor holding shaders/):
#   bin/aether-editor(.exe), bin/aether-player(.exe)  (+ the MSVC runtime DLLs on Windows)
#   shaders/                     compiled at startup (pipeline cache under the user's .aether/cache)
#   content/                     starter content, copied to Documents/Aether Projects on first run
#   licenses/<component>/        third-party license texts
#   aether-install.json          install marker: user data never goes into the install folder
#   README.md, THIRD_PARTY_NOTICES.md
# Package the Release configuration (debug builds need the Vulkan SDK's validation layers and
# non-redistributable debug runtimes).

include(GNUInstallDirs)

set(_ae_apps)
foreach(_t aether-editor aether-player)
    if(TARGET ${_t})
        list(APPEND _ae_apps ${_t})
    endif()
endforeach()
if(NOT _ae_apps)
    return() # nothing to ship (module filter excluded the apps)
endif()

# Everything we ship is in the "Aether" component; third-party install rules (headers / static libs
# of glfw, Jolt, glm, ...) land in other components and are never packaged:
#   cmake --install <build> --component Aether --prefix <dir>
set(AE_INSTALL_COMPONENT Aether)
install(TARGETS ${_ae_apps} RUNTIME DESTINATION bin COMPONENT ${AE_INSTALL_COMPONENT})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/" DESTINATION shaders COMPONENT ${AE_INSTALL_COMPONENT})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/content/" DESTINATION content COMPONENT ${AE_INSTALL_COMPONENT}
        PATTERN "*.py" EXCLUDE        # authoring tools, not content
        PATTERN ".*" EXCLUDE)
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/docs/THIRD_PARTY_NOTICES.md" DESTINATION .
        COMPONENT ${AE_INSTALL_COMPONENT})
configure_file("${PROJECT_SOURCE_DIR}/cmake/packaging/aether-install.json.in"
               "${PROJECT_BINARY_DIR}/aether-install.json" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/aether-install.json" DESTINATION . COMPONENT ${AE_INSTALL_COMPONENT})

# Third-party license texts, straight from the fetched sources.
foreach(_dep vulkan_headers volk vma glfw glm glslang imgui imguizmo entt json cgltf stb mikktspace
             bc7enc_rdo meshoptimizer joltphysics lua sol2)
    if(DEFINED ${_dep}_SOURCE_DIR AND EXISTS "${${_dep}_SOURCE_DIR}")
        file(GLOB _lic LIST_DIRECTORIES false "${${_dep}_SOURCE_DIR}/LICENSE*" "${${_dep}_SOURCE_DIR}/COPYING*"
             "${${_dep}_SOURCE_DIR}/license*" "${${_dep}_SOURCE_DIR}/copying*")
        if(_dep STREQUAL "mikktspace") # the zlib notice lives in the header
            list(APPEND _lic "${${_dep}_SOURCE_DIR}/mikktspace.h")
        elseif(_dep STREQUAL "lua" AND NOT _lic) # the MIT notice lives at the end of lua.h
            list(APPEND _lic "${${_dep}_SOURCE_DIR}/lua.h")
        endif()
        if(_lic)
            install(FILES ${_lic} DESTINATION licenses/${_dep} COMPONENT ${AE_INSTALL_COMPONENT})
        endif()
    endif()
endforeach()

# Windows: ship the MSVC runtime DLLs next to the executables (app-local, no redistributable
# installer needed). The Universal CRT is part of Windows 10/11.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT ${AE_INSTALL_COMPONENT})
    include(InstallRequiredSystemLibraries)
endif()

# ---- CPack --------------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "Aether")
set(CPACK_PACKAGE_VENDOR "Aether")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Aether - a real-time 3D engine and editor")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Aether")
set(CPACK_PACKAGE_EXECUTABLES "aether-editor" "Aether Editor")   # Start menu entry
set(CPACK_CREATE_DESKTOP_LINKS "aether-editor")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
# Only the Aether component, as one package (no component selection page in the installer).
set(CPACK_COMPONENTS_ALL ${AE_INSTALL_COMPONENT})
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON) # unzips into Aether-<version>-<os>/
set(CPACK_COMPONENT_AETHER_DISPLAY_NAME "Aether Editor and Player")
set(CPACK_COMPONENT_AETHER_REQUIRED ON)
set(CPACK_COMPONENT_AETHER_HIDDEN ON)
if(WIN32)
    set(_ae_os "windows-x64")
else()
    set(_ae_os "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    string(TOLOWER "${_ae_os}" _ae_os)
endif()
set(CPACK_PACKAGE_FILE_NAME "Aether-${PROJECT_VERSION}-${_ae_os}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP") # portable: unzip anywhere and run bin/aether-editor.exe
    # Inno Setup (free: https://jrsoftware.org/isinfo.php, or `winget install JRSoftware.InnoSetup`)
    # builds AetherSetup: per-user install (no admin rights), Start menu + desktop shortcuts,
    # uninstaller, "launch the editor" on the last page. CMake >= 3.27 has the generator.
    set(_pf86 "ProgramFiles(x86)")
    find_program(AE_ISCC_EXECUTABLE ISCC
                 PATHS "$ENV{${_pf86}}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6"
                       "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6")
    if(AE_ISCC_EXECUTABLE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
        list(APPEND CPACK_GENERATOR "INNOSETUP")
        set(CPACK_INNOSETUP_EXECUTABLE "${AE_ISCC_EXECUTABLE}")
        set(CPACK_INNOSETUP_ARCHITECTURE "x64")
        set(CPACK_INNOSETUP_USE_MODERN_WIZARD ON)
        set(CPACK_INNOSETUP_IGNORE_LICENSE_PAGE ON)
        set(CPACK_INNOSETUP_IGNORE_README_PAGE ON)
        set(CPACK_INNOSETUP_RUN_EXECUTABLES "aether-editor")
        set(CPACK_INNOSETUP_CREATE_UNINSTALL_LINK ON)
        set(CPACK_INNOSETUP_SETUP_PrivilegesRequired "lowest")               # per-user install
        set(CPACK_INNOSETUP_SETUP_PrivilegesRequiredOverridesAllowed "dialog") # or all users
        set(CPACK_INNOSETUP_SETUP_AppId "{{7C1E1C5A-4F1B-4B7E-9C2E-AE7E7AE7A001}")
        message(STATUS "Aether: packaging with ZIP + Inno Setup (${AE_ISCC_EXECUTABLE})")
    else()
        message(STATUS "Aether: packaging with ZIP (install Inno Setup 6 for AetherSetup.exe)")
    endif()
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
