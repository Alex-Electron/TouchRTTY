#pragma once

// Polls USB CDC serial for incoming characters and processes complete commands.
// Call from Core 1 main loop.
void handle_serial_commands();
