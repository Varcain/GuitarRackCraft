# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# =============================================================================
# cmake/modules/WatchSources.cmake — Source file tracking for ExternalProject
#
# Adds a watch_sources step to an ExternalProject target that makes the build
# step depend on actual source files. This enables file-level incremental
# rebuilds: touching a source file triggers only the affected target's rebuild.
#
# Usage:
#   watch_external_sources(serd DIRECTORIES "${THIRD_PARTY}/serd/src")
#   watch_external_sources(mesa DIRECTORIES "${THIRD_PARTY}/mesa/src"
#                               PATTERNS "*.c" "*.h")
# =============================================================================

function(watch_external_sources target)
    cmake_parse_arguments(ARG "" "" "DIRECTORIES;PATTERNS" ${ARGN})
    if(NOT ARG_PATTERNS)
        set(ARG_PATTERNS "*.c" "*.h" "*.cpp" "*.hpp" "*.cc")
    endif()
    set(_all_sources)
    foreach(_dir IN LISTS ARG_DIRECTORIES)
        foreach(_pat IN LISTS ARG_PATTERNS)
            file(GLOB_RECURSE _found "${_dir}/${_pat}")
            list(APPEND _all_sources ${_found})
        endforeach()
    endforeach()
    if(_all_sources)
        ExternalProject_Add_Step(${target} watch_sources
            DEPENDERS build
            DEPENDS ${_all_sources}
        )
    endif()
endfunction()
