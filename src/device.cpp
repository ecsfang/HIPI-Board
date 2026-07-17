#include <stdio.h>
#include "hpil.h"

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
    LOGF("\r\n@@@ " HILIGHT "%s" RESET " status:%s addr:%d sai:%s nSai:%d nAau:%d sdi:%02X",
        name(), isTalker() ? "TALKER" : ((isListener()) ? "LISTENER" : " "),
        addr(), m_sai?"TRUE":"FALSE", m_nSai, m_nAau, m_sdi?*m_sdi:0);
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
        addr(m_nAau);
        return true;
    }
    if( cmd == SAI && isTalker() ) {
        *rtn = m_nSai;
        m_sai = true;
        return true;
    }
    if( cmd == SDI && isTalker() ) {
        m_sdi = name();
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
