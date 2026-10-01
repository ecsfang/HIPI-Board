// boot_service.h -- keeps the USB link and the PC's PILBox commands
// answered while HIPI is still starting up (display set-up, SD card,
// bitmaps...), so a PC program such as pyILPER that connects meanwhile gets
// its answers in time. Called from the start-up's longer waits and loops;
// does nothing once start-up is over (hipi_bootServiceDone()).
#pragma once

void hipi_bootService();
void hipi_bootServiceDone();
