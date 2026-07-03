#include <stdio.h>
#include "hpil.h"
#include "usb_serial.h"

#define printf cdc0_printf

void CDevice::show(void)
{
    printf("%s: status:%c addr:%2d", devName, isTalker()?'T':((isListener())?'L':'-'), addr());
}

bool CDevice::base(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    IL_ADDR_t   _addr = GET_ADDR(cmd);
    if( _bPrt ) printf("[");
    if( ((cmd == UNL) && isListener())
            || ((cmd == UNT) && isTalker()) ) {
        if( _bPrt ) {
            printf("1] ");
            if( isListener() )
                printf("UNL (%d)\r\n", addr());
            if( isTalker() )
                printf("UNT (%d)\r\n", addr());
        }
        setIdle();
        return true;
    }
    if ((cmd == DCL) || ((cmd == SDC) && isListener()) ) {
        clear();
        setIdle();
        if( _bPrt ) printf("2] ");
        return true;
    }
    if( cmd == AAU ) {
        _addr = nAau;
        if( _bPrt ) printf("3] ");
        return true;
    }
    if( inAddrRange(cmd, LAD) ) {
        if( _bPrt ) printf("4] ");
        if( addr() == _addr && _addr < 31 ) {
            if( _bPrt ) printf("LISTENER: addr=%d\r\n", addr());
            setListener();
        } else
            setIdle();
        return true;
    }
    if( inAddrRange(cmd, TAD) ) {
        if( _bPrt ) printf("5] ");
        if( addr() == _addr ) {
            if( _bPrt ) printf("TALKER: addr=%d\r\n", addr());
            setTalker();
        } else
            setIdle();
        return true;
    }
    if( inAddrRange(cmd, AAD) ) {
        addr(cmd - AAD);
        *rtn = cmd + 1;
        if( _bPrt ) printf("6] ");
        return true;
    }
    if( sai ) {
        *rtn = (cmd == nSai) ? ETO : ETE;
        sai = false;
        if( _bPrt ) printf("7] ");
        return true;
    }
    if( sdi ) {
        *rtn = *sdi++;
        if( *rtn == 0 ) {
            *rtn = ETO;
            sdi = NULL;
        }
        if( _bPrt ) printf("8] ");
        return true;
    }

    if( _bPrt ) printf("-] ");
    return false;
}
