#ifndef __HPIL_H__
#define __HPIL_H__

#define DOE     0x400
#define SDC     0x404
#define DCL     0x414
#define LAD     0x420
#define UNL     0x43F
#define TAD     0x440
#define UNT     0x45F
#define IFC     0x490
#define AAU     0x49A
#define DDL     0x4A0
#define DDT     0x4C0
#define ETO     0x540
#define ETE     0x541
#define NRD     0x542
#define SDA     0x560
#define SST     0x561
#define SDI     0x562
#define SAI     0x563
#define AAD     0x580

#define MAX_ADDR    0x1F
#define inAddrRange(a,x) ((a) >= (x) && (a) <= ((x)+MAX_ADDR))


typedef unsigned short int IL_CMD_t;
typedef unsigned char      IL_ADDR_t;

typedef enum {
    STAT_NONE,
    LISTENER,
    TALKER
} IL_Status_e;

typedef enum {
    MODE_NONE,
    P_MODE
} IL_Mode_e;

#endif//__HPIL_H__