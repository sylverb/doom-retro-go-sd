#pragma once
// I2C two-player networking (RP2040-specific). All call sites are guarded by
// #if USE_PICO_NET, which this port does not define; the header only needs to
// exist for the unconditional #include.
