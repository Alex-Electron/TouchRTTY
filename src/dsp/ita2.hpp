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
