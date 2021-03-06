project(focal)
cmake_minimum_required(VERSION 2.8)

add_definitions(-D_GNU_SOURCE)
set(CMAKE_C_FLAGS_DEBUG "-g -Wall -Werror -Wno-pointer-sign")

find_package(PkgConfig REQUIRED)

# GTK3
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
include_directories(${GTK3_INCLUDE_DIRS})
link_directories(${GTK3_LIBRARY_DIRS})
add_definitions(${GTK3_CFLAGS_OTHER})

# LibXML2
find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})
add_definitions(${LIBXML2_DEFINITIONS})

# cURL
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})
add_definitions(${LIBXML2_DEFINITIONS})

# libical
pkg_check_modules(ICAL REQUIRED libical)
include_directories(${ICAL_INCLUDE_DIRS})

# libsecret
pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
include_directories(${LIBSECRET_INCLUDE_DIRS})

# json-glib
pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
include_directories(${JSONGLIB_INCLUDE_DIRS})

# executable
add_executable(${PROJECT_NAME}
	src/account-edit-dialog.c
	src/accounts-dialog.c
	src/app-header.c
	src/async-curl.c
	src/caldav-calendar.c
	src/calendar.c
	src/calendar-config.c
	src/cell-renderer-attendee-action.c
	src/cell-renderer-attendee-partstat.c
	src/date-selector-button.c
	src/event.c
	src/event-panel.c
	src/event-popup.c
	src/ics-calendar.c
	src/main.c
	src/oauth2-provider.c
	src/oauth2-provider-google.c
	src/oauth2-provider-outlook.c
	src/outlook-calendar.c
	src/reminder.c
	src/remote-auth-basic.c
	src/remote-auth.c
	src/remote-auth-oauth2.c
	src/time-spin-button.c
	src/week-view.c
)

target_link_libraries(${PROJECT_NAME}
	${GTK3_LIBRARIES}
	${LIBXML2_LIBRARIES}
	${CURL_LIBRARIES}
	${ICAL_LIBRARIES}
	${LIBSECRET_LIBRARIES}
	${JSONGLIB_LIBRARIES}
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(FILES res/focal.desktop DESTINATION share/applications)
