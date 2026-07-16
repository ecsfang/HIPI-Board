#ifndef __HPIL_H__
#define __HPIL_H__

#include "usb_serial.h"

#define RESET           "\e[0m"
#define HILIGHT         "\e[1;92m"       // Green highlight

#define DOE     0x000   // Data byte (0-255)
#define CMD     0x400   // Command
#define SDC     0x404   // Selected device clear
#define DCL     0x414
#define LAD     0x420
#define UNL     0x43F
#define TAD     0x440
#define UNT     0x45F
#define IFC     0x490
#define AAU     0x49A   // Auto Address Unconfigure
#define DDL     0x4A0   // Device dependent listener (0-31)
#define DDT     0x4C0   // Device dependent talker (0-31)
#define ETO     0x540
#define ETE     0x541
#define NRD     0x542
#define SDA     0x560   // Send data
#define SST     0x561   // Send status
#define SDI     0x562   // Send device ID
#define SAI     0x563   // Send accessory identification
#define AAD     0x580
#define RFC     0x500
#define IDY     0x600
#define LLO     0x411
#define LPD     0x49D

#define DOE_MASK   0x400    // 0xx xxxx xxxx
#define IS_DATA(x) (((x) & DOE_MASK) == DOE)

#define CMD_MASK   0x700    // 100 xxxx xxxx
#define IS_CMD(x)  (((x) & CMD_MASK) == CMD)

#define IDLE_FRAME (IDY | 0xC0)
//#define IS_IDLE(x) ((x) == IDLE_FRAME)
#define IS_IDLE(x) (((x) & 0x700) == IDY)

// info below from Christoph Giesselink:
// PILBox commands
#define TDIS    0x494   // TDIS
#define COFI    0x495   // COFF with IDY, firmware >= v1.6, not used if no IDY frames are supported
#define CON     0x496   // Controller ON, not used, HP41 can only be controller (check HP-IL DEVELOPMENT ROM Scope function!)
#define COFF    0x497   // Controller OFF
#define SSRQ    0x49C   // Set Service Request, obsolete, not used on HP41
#define CSRQ    0x49D   // Clear Service Request, obsolete, not used on HP41

// Channel for the HP-IL port for PIL-Box interface
#define ITF_HPIL   1

#define MAX_ADDR    0x1F
#define MAX_CMD     0x1F
#define GET_ADDR(x) (x&MAX_ADDR)

#define NO_FRAME 0xFFFF

#define inAddrRange(a,x) ((a) >= (x) && (a) <= ((x)+MAX_ADDR))

typedef unsigned short int IL_CMD_t;
typedef unsigned char      IL_ADDR_t;
typedef unsigned char      IL_DATA_t;

extern char *ilMnemonic(IL_CMD_t frame, char *buf);

typedef enum {
    STAT_IDLE,
    LISTENER,
    TALKER
} IL_Status_e;

typedef enum {
    NONE,
    DISPLAY,
    DRIVE,
    LED,
    PILBOX
} IL_Type_e;

class CDevice {
protected:
    IL_Status_e     m_status;
    IL_ADDR_t       m_addr;
    const char      *m_devName;
    bool            m_sai;
    IL_ADDR_t       m_nSai;
    IL_ADDR_t       m_nAau;
    const char      *m_sdi;
    IL_Type_e       m_type;
public:
    CDevice(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau, IL_Type_e type = NONE) {
        m_devName = name;
        m_status = STAT_IDLE;
        m_addr = 31;
        m_sai = false;
        m_sdi = NULL;
        m_nSai = _sai;
        m_nAau = _aau;
        m_type = type;
    }
    bool base(IL_CMD_t cmd, IL_CMD_t *rtn);
    virtual IL_CMD_t hpil(IL_CMD_t cmd);
    virtual void doListener(IL_CMD_t cmd, IL_CMD_t *rtn) {}
    virtual void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn) {}
    virtual void clear() = 0;
    virtual void idle(void) {}
    virtual void ifc(void) {}
    virtual void preProc(IL_CMD_t c) {}
    virtual void show(void);
    void addr(IL_CMD_t a) { m_addr = a; }
    IL_CMD_t addr() { return m_addr; }
    void status(IL_Status_e s) { m_status = s; }
    IL_Status_e status() { return m_status; }
    bool isStatus(IL_Status_e s) { return status() == s; }
    void setIdle() { status(STAT_IDLE); }
    bool isIdle() { return isStatus(STAT_IDLE); }
    void setTalker() { status(TALKER); }
    bool isTalker() { return isStatus(TALKER); }
    void setListener() { status(LISTENER); }
    bool isListener() { return isStatus(LISTENER); }
    const char *name() { return m_devName; }
    void type(IL_Type_e type) { m_type = type; }
    IL_Type_e type(void) { return m_type; }
};

#endif//__HPIL_H__