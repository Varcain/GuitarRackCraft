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
# cmake/modules/WriteIfChanged.cmake
# =============================================================================

# grc_write_if_changed(<file> <content>...)
#   file(WRITE) - the content arguments are concatenated the same way - but
#   leaves <file> alone when it already holds that content. For every file
#   configure generates that the build reads (headers, scripts, cross files,
#   TTLs): rewriting an unchanged one bumps its mtime, and everything that
#   includes it rebuilds after every configure.
function(grc_write_if_changed path)
    # ARGV<n>, not ARGN: an argument's own semicolons (C code) must survive.
    set(_content "")
    if(ARGC GREATER 1)
        math(EXPR _last "${ARGC} - 1")
        foreach(_i RANGE 1 ${_last})
            string(APPEND _content "${ARGV${_i}}")
        endforeach()
    endif()
    if(EXISTS "${path}")
        file(READ "${path}" _current)
        if(_current STREQUAL _content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${_content}")
endfunction()
