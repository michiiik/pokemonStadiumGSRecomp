if (ANDROID AND TARGET SDL2-static)
    set(SDL2_FOUND TRUE)
    set(SDL2_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/lib/SDL2/include")
    set(SDL2_LIBRARIES SDL2-static)
    set(SDL2_VERSION_STRING "2.30.3")
    return()
endif()

set(SDL2_FOUND FALSE)
