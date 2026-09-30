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
# cmake/plugins.cmake — the LV2 plugins the native prebuild builds
#
# One grc_plugin() per cmake/targets/<name>.cmake. That file builds the
# plugin(s), stages their libraries into ${JNILIBS_DIR} and defines the target
# <name>_done. The rest is derived from this list: what CMakeLists.txt
# includes and all_plugins depends on, what the metadata step waits for and
# which bundles it describes (GeneratePluginMetadata.cmake reads this file
# too), and the Play asset pack the libraries ship in.
#
# grc_plugin(<name> PACK <gx|neural|brummer> [AUTHOR <author> BUNDLES <bundle>...])
#   PACK     the asset pack its libraries go to (playstore flavor; the full
#            flavor ships them all in its jniLibs)
#   BUNDLES  its LV2 bundles, assets/lv2/<bundle>.lv2, listed in
#            plugin_metadata.json with AUTHOR as their author. The bundles of
#            the GxPlugins.lv2 collection are scanned separately.
#
# Plain properties only, so script-mode (cmake -P) code can include it.
# =============================================================================

function(grc_plugin name)
    cmake_parse_arguments(ARG "" "PACK;AUTHOR" "BUNDLES" ${ARGN})
    if(NOT ARG_PACK MATCHES "^(gx|neural|brummer)$")
        message(FATAL_ERROR "grc_plugin(${name}): PACK must be gx, neural or brummer")
    endif()
    if(ARG_BUNDLES AND NOT ARG_AUTHOR)
        message(FATAL_ERROR "grc_plugin(${name}): BUNDLES needs an AUTHOR")
    endif()
    set_property(GLOBAL APPEND PROPERTY GRC_PLUGINS "${name}")
    set_property(GLOBAL PROPERTY GRC_PLUGIN_${name}_PACK "${ARG_PACK}")
    foreach(_bundle IN LISTS ARG_BUNDLES)
        set_property(GLOBAL APPEND PROPERTY GRC_PLUGIN_BUNDLES "${_bundle}|${ARG_AUTHOR}")
    endforeach()
endfunction()

# guitarix: GxPlugins.lv2, the guitarix trunk plugins, and their X11 UIs
grc_plugin(gx_plugins     PACK gx)
grc_plugin(trunk_plugins  PACK gx)
grc_plugin(plugin_uis     PACK gx)

# Neural amp models
grc_plugin(nam            PACK neural  AUTHOR "Neural Amp Modeler" BUNDLES neural_amp_modeler)
grc_plugin(aidax          PACK neural  AUTHOR "Aida DSP"  BUNDLES aidadsp)
grc_plugin(aidax_full     PACK neural  AUTHOR "Aida DSP"  BUNDLES AIDA-X)
grc_plugin(neuralrack     PACK neural  AUTHOR brummer10   BUNDLES Neuralrack)

# brummer10
grc_plugin(impulseloader  PACK brummer AUTHOR brummer10   BUNDLES ImpulseLoader)
grc_plugin(xdarkterror    PACK brummer AUTHOR brummer10   BUNDLES XDarkTerror)
grc_plugin(xtinyterror    PACK brummer AUTHOR brummer10   BUNDLES XTinyTerror)
grc_plugin(collisiondrive PACK brummer AUTHOR brummer10   BUNDLES CollisionDrive)
grc_plugin(metaltone      PACK brummer AUTHOR brummer10   BUNDLES MetalTone)
grc_plugin(gxcabsim       PACK gx      AUTHOR brummer10   BUNDLES GxCabSim)
grc_plugin(modamptk       PACK brummer AUTHOR brummer10
           BUNDLES PreAmps PowerAmps PreAmpImpulses PowerAmpImpulses)
grc_plugin(fatfrog        PACK brummer AUTHOR brummer10   BUNDLES FatFrog)

# Our own
grc_plugin(doubletracker  PACK gx      AUTHOR Varcain     BUNDLES doubletracker)
