add_library(herald
        src/herald_ward_manager.cpp
        src/herald_http_server.cpp
)

target_include_directories(herald
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(herald
        PUBLIC
            karshipta_proto
            # herald_ward_manager.h includes ward_manager.h (its constructor
            # takes a WardManager& to check has_ward()), so ward is part of
            # herald's own public interface, not just its implementation,
            # same reasoning fleet/CMakeLists.txt documents for its own
            # WardManager& dependency.
            ward
            transport
        PRIVATE
            karshipta_warnings
            yaml-cpp::yaml-cpp
            httplib::httplib
)
