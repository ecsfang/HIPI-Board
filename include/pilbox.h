#ifndef __PILBOX_H__
#define __PILBOX_H__

#include "hpil.h"

#define NO_FRAME 0xFFFF

extern IL_CMD_t ILBOX_ReceiveFrame(void);
extern IL_CMD_t ILBOX_SendFrame(IL_CMD_t wFrame);

#endif//__PILBOX_H__