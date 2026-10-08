/* hipi_features.h -- build-time switches for optional features. Plain C
 * (also included by tusb_config.h, which TinyUSB's C sources use).
 *
 * The display mirror (display_mirror.h, tools/hipiview) has two parts that
 * can be switched off separately:
 *   HIPI_MIRROR_PORT       the fourth USB serial port, "HIPI Display Mirror"
 *                          (0: the three ports as before)
 *   HIPI_MIRROR_TRANSPORT  the display's SPI traffic passed through the
 *                          mirror encoder (0: straight to the display)
 * Mirroring needs both; the 5" build never mirrors (only the port exists).
 */
#ifndef HIPI_FEATURES_H
#define HIPI_FEATURES_H

#ifndef HIPI_MIRROR_PORT
#define HIPI_MIRROR_PORT       1
#endif

#ifndef HIPI_MIRROR_TRANSPORT
#define HIPI_MIRROR_TRANSPORT  1
#endif

#endif
