cmake_minimum_required( VERSION 3.13 )

project( uxplay )

set(BBT_AIRPLAY_VERSION "0.1.0" CACHE STRING "BigBirdTouch AirPlay receiver version")
set(BBT_UPSTREAM_REVISION "3ca7526387e894d6848b84c209de361c3bedd1ec" CACHE STRING "Pinned UxPlay revision")

message( STATUS "Project name: " ${PROJECT_NAME} )

include(GNUInstallDirs)

set ( CMAKE_CXX_STANDARD 11 )

if (ZOOMFIX )
  message (STATUS "cmake option ZOOMFIX is no longer used (if needed, ZOOMFIX is automatically applied if X11 libraries are present)" )
endif()

if ( ( UNIX AND NOT APPLE ) OR USE_X11 )
  if ( NOT NO_X11_DEPS )
  find_package( X11  )
    if ( X11_FOUND )
      message (STATUS "Will compile using X11 Libraries  (use cmake option -DNO_X11_DEPS=ON if X11 dependence is not wanted)" )
      link_libraries( ${X11_LIBRARIES} )
      include_directories( ${X11_INCLUDE_DIR} )
    else ()
      message (STATUS "X11 libraries not found, will compile without X11 dependence" )
    endif ()
  else()
    message (STATUS "will compile without X11 dependence" )
  endif()
endif()

if( UNIX AND NOT APPLE )
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS dbus-1>=1.4.12)
  if (DBUS_FOUND )
      add_definitions(-DDBUS )
      include_directories(${DBUS_INCLUDE_DIRS})
      #FreeBSD has this Library in /usr/local/lib
      find_library (DBUS_LIBRARY NAMES dbus-1 PATHS /usr/local/lib)
      if (NOT DBUS_LIBRARY)
      	 message(FATAL_ERROR "libdbus-1 not found")
      endif()
  endif()
endif()

if( UNIX AND NOT APPLE )
  # convert AirPlay colormap 1:3:7:1 to sRGB (1:1:7:1), needed on Linux and BSD
  add_definitions( -DFULL_RANGE_RGB_FIX )
else()
  set( CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE )
endif()

if (USE_DNS_SD OR ( APPLE AND NOT USE_MDNS ))
  message ( STATUS "******** using Bounjour/Apple dns_sd.h for Service Discovery")
  add_subdirectory( lib/dns_sd)
  if (UNIX AND NOT APPLE)
    add_definitions( -DSUPPRESS_AVAHI_COMPAT_WARNING )
  endif()	     
  if ( APPLE )	
       message ( STATUS "******** use \"cmake -DUSE_MDNS=1\" to build using UxPlay mdnsd code instead of Apple Bonjour" )	
  endif()
else() 
  message ( STATUS "******** using internal minimal mDNSResponder implementation (lib/mdnsd) for Service Discovery")
  if (NOT APPLE)
     message ( STATUS "******** use \"cmake -DUSE_DNS_SD=1\" to build with the previously-used Apple/Avahi dns-sd.h code")
  endif()
  add_subdirectory( lib/mdnsd)
endif()

add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )

find_package( PkgConfig REQUIRED )
pkg_check_modules( GLIB2 REQUIRED glib-2.0 )

add_executable( uxplay uxplay.cpp bbap_writer.cpp bbap_renderer_stubs.cpp )
target_compile_definitions(uxplay PRIVATE
  BBT_AIRPLAY_VERSION="${BBT_AIRPLAY_VERSION}"
  BBT_UPSTREAM_REVISION="${BBT_UPSTREAM_REVISION}"
)
target_include_directories( uxplay PRIVATE ${GLIB2_INCLUDE_DIRS} )
target_link_directories( uxplay PRIVATE ${GLIB2_LIBRARY_DIRS} )
target_link_libraries( uxplay airplay ${GLIB2_LINK_LIBRARIES} )
set_target_properties( uxplay PROPERTIES OUTPUT_NAME "bbt_airplay_receiver" )
if (DBUS_FOUND)
      target_link_libraries( uxplay ${DBUS_LIBRARY})
endif()

install( TARGETS  uxplay RUNTIME DESTINATION bin )
install( FILES uxplay.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES README.md README.txt README.html LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} ) 
install( FILES lib/llhttp/LICENSE-MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp )
install( FILES uxplay.service DESTINATION ${CMAKE_INSTALL_DOCDIR}/systemd ) 
install( FILES Bluetooth_LE_beacon/uxplay-beacon.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES Bluetooth_LE_beacon/uxplay-beacon.py
         DESTINATION bin
         PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ  WORLD_EXECUTE WORLD_READ)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_BleuIO.py
         DESTINATION bin
         PERMISSIONS  OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)

if (DBUS_FOUND)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_BlueZ.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()

if (WIN32)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_winrt.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()

if (UNIX AND NOT APPLE)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_HCI.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()


# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
