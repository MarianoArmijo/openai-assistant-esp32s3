cmake_minimum_required(VERSION 3.19)

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()

  add_compile_definitions(WIFI_SSID="$ENV{WIFI_SSID}")
  add_compile_definitions(WIFI_PASSWORD="$ENV{WIFI_PASSWORD}")
endif()

if(NOT DEFINED ENV{OPENAI_API_KEY})
  message(FATAL_ERROR "Env variable OPENAI_API_KEY must be set")
endif()

if(DEFINED ENV{LOG_DATACHANNEL_MESSAGES})
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
endif()

add_compile_definitions(OPENAI_API_KEY="$ENV{OPENAI_API_KEY}")
add_compile_definitions(OPENAI_MODEL="gpt-realtime-mini")
add_compile_definitions(OPENAI_REALTIMEAPI="https://api.openai.com/v1/realtime?model=gpt-realtime-mini")

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "components/esp-protocols/components/esp_websocket_client")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(src)
