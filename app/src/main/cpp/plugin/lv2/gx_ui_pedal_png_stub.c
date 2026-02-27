/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * UNUSED: We now embed pedal.png via ld -r -b binary / llvm-objcopy -I binary
 * in CMakeLists.txt (same as guitarix xputty/resources/wscript). See
 * plugin/lv2/pedal.png and docs/BINARY_PEDAL_PNG.md.
 *
 * This file is kept only as reference for the symbol layout and minimal PNG.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Minimal 1x1 pixel grey PNG, 68 bytes. Symbols match ld -r -b binary output. */
const unsigned char _binary_pedal_png_start[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41,
    0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
    0x00, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18,
    0xdd, 0x8d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

/* _end = one past last byte so size = _end - _start (plugin uses LDLEN for stream size) */
const unsigned char *const _binary_pedal_png_end = _binary_pedal_png_start + sizeof(_binary_pedal_png_start);

#ifdef __cplusplus
}
#endif
