include(FetchContent)

set(SDP_JSON_ARCHIVE "" CACHE FILEPATH "Local copy of the nlohmann/json 3.12.0 release archive")
if(NOT SDP_JSON_ARCHIVE)
    set(SDP_JSON_ARCHIVE "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz")
endif()
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL "${SDP_JSON_ARCHIVE}"
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(nlohmann_json)
