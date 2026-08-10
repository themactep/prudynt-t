#pragma once

namespace VideoPrivacyControl {

void run();

// Apply persisted privacy state on startup (before streams go live).
// Reads privacy.enabled from config and engages the cover if true.
void applyStartupState();

}
