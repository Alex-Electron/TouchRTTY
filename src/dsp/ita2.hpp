// TouchRTTY
// Copyright (C) 2026 Alexander Lavrinovich (Alex.Electron)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Baudot / ITA2 5-bit character tables (LETTERS and FIGURES)
// Index 27 = shift to FIGURES, 31 = shift to LETTERS

const char ita2_ltrs[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
    'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0'
};

const char ita2_figs[32] = {
    '\0', '3', '\n', '-', ' ', '\'', '8', '7',
    '\r', '$', '4', '\'', ',', '!', ':', '(',
    '5', '\"', ')', '2', '#', '6', '0', '1',
    '9', '?', '&', '\0', '.', '/', '=', '\0'
};
