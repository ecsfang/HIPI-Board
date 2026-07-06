#include <stdio.h>
#include "hpil.h"

#define printf cdc0_printf


IL_CMD_t CDevice::hpil(IL_CMD_t cmd)
{
    IL_CMD_t rtn = cmd;

    // Handle all base commands
    if( base(cmd, &rtn) ) {
        return rtn;
    }

    // Otherwise handle device specific commands
    if( isTalker() ) {
        doTalker(cmd, &rtn);
    } else if( isListener() ) {
        doListener(cmd, &rtn);
    }
    return rtn;
}

void CDevice::show(void)
{
    printf("%s: status:%c addr:%2d", m_devName, isTalker()?'T':((isListener())?'L':'-'), addr());
}

bool CDevice::base(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    IL_ADDR_t   _addr = GET_ADDR(cmd);

    preProc(cmd);

    if( cmd == IFC) {
        ifc();
    }
    if( (cmd == UNL && isListener())
            || (cmd == UNT && isTalker()) ) {
        setIdle();
        return true;
    }
    if ((cmd == DCL) || ((cmd == SDC) && isListener()) ) {
        clear();
        setIdle();
        return true;
    }
    if( cmd == AAU ) {
        _addr = m_nAau;
        return true;
    }
    if( cmd == SAI && isTalker() ) {
        *rtn = m_nSai;
        m_sai = true;
        return true;
    }
    if( cmd == SDI && isTalker() ) {
        m_sdi = m_devName;
        *rtn = *m_sdi++;
        return true;
    }
    if( inAddrRange(cmd, LAD) ) {
        if( addr() == _addr && _addr < 31 ) {
            setListener();
        } else
            setIdle();
        return true;
    }
    if( inAddrRange(cmd, TAD) ) {
        if( addr() == _addr ) {
            setTalker();
        } else
            setIdle();
        return true;
    }
    if( inAddrRange(cmd, AAD) ) {
        addr(cmd - AAD);
        *rtn = cmd + 1;
        return true;
    }
    if( m_sai ) {
        *rtn = (cmd == m_nSai) ? ETO : ETE;
        m_sai = false;
        return true;
    }
    if( m_sdi ) {
        *rtn = *m_sdi++;
        if( *rtn == 0 ) {
            *rtn = ETO;
            m_sdi = NULL;
        }
        return true;
    }
    // Command not handled here, send to the device
    return false;
}
