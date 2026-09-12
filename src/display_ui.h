#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Display + touch initialization and all on-screen rendering.
// ---------------------------------------------------------------------------

// Brings up the ST77922 panel, the TFT_eSPI sprite used as the framebuffer and
// the capacitive touch controller.
void initDisplayUi();

// Raw touch state (true while the panel is pressed).
bool readTouch();

// Full-screen transient message (boot status, Wi-Fi problems, fatal errors).
void uiMessage(const String &message);

// Updates only the status line, keeping the current conversation on screen.
void uiStatus(const String &status);

// Renders the status line plus the transcription and the answer.
void uiConversation(const String &status, const String &user, const String &assistant);
