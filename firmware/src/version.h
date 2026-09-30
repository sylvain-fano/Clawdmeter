#pragma once

// Firmware version shown on the About page. Release builds get the git tag
// from CI (-DFW_VERSION="v1.2.3"); local builds say "dev".
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
