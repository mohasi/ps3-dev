#pragma once
#include <stdint.h>

// Read the inserted audio CD's Table of Contents from the optical drive (ATAPI READ TOC via the storage
// syscalls). Fills frameOffsets[i] with track i's CDDB frame offset (drive LBA + 150 lead-in) and sets
// *leadoutFrame to the lead-out's frame offset. Returns the track count (>=1) or <=0 on failure.
// Modeled on webMAN-MOD's cobra_get_cd_td (hb-samples/webMAN-MOD/cobra).
int readCdToc(uint32_t *frameOffsets, int maxTracks, uint32_t *leadoutFrame);
