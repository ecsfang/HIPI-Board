#include <stdio.h>
#include "hpil.h"

void CDevice::show(void)
{
    printf("%s: status:%c addr:%2d", devName, status==2?'T':((status==1)?'L':'-'), addr);
}

bool CDevice::base(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    IL_ADDR_t   _addr = GET_ADDR(cmd);
    if( _bPrt ) printf("[");
    if( ((cmd == UNL) && (status == LISTENER))
            || ((cmd == UNT) && (status == TALKER)) ) {
        if( _bPrt ) {
            printf("1] ");
            if( status == LISTENER )
                printf("UNL (%d)\n", addr);
            if( status == TALKER )
                printf("UNT (%d)\n", addr);
        }
        status = STAT_IDLE;
        return true;
    }
    if ((cmd == DCL) || ((cmd == SDC) && (status == LISTENER)) ) {
        clear();
        status = STAT_IDLE;
        if( _bPrt ) printf("2] ");
        return true;
    }
    if( cmd == AAU ) {
        addr = nAau;
        if( _bPrt ) printf("3] ");
        return true;
    }
    if( inAddrRange(cmd, LAD) ) {
        if( _bPrt ) printf("4] ");
        if( addr == _addr && _addr < 31 ) {
            if( _bPrt ) printf("LISTENER: addr=%d\n", addr);
            status = LISTENER;
        } else
            status = STAT_IDLE;
        return true;
    }
    if( inAddrRange(cmd, TAD) ) {
        if( _bPrt ) printf("5] ");
        if( addr == _addr ) {
            if( _bPrt ) printf("TALKER: addr=%d\n", addr);
            status = TALKER;
        } else
            status = STAT_IDLE;
        return true;
    }
    if( inAddrRange(cmd, AAD) ) {
        addr = cmd - AAD;
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
