#include <stdio.h>
#include "display.h"

static char *devName = "CVIDEOx";

IL_CMD_t CDisplay::hpil(IL_CMD_t cmd)
{
    IL_CMD_t  rtn = cmd;

    if( cmd == IFC || ((cmd == UNL) && (status == LISTENER)) || ((cmd == UNT) && (status == TALKER)) ) {
        status = STAT_NONE;
    } else if ((cmd == DCL) || ((cmd == SDC) && (status == LISTENER)) ) {
        /* Clear device 
            video.clear()
        */
        fifo = std::queue<unsigned char>();
        status = STAT_NONE;
    } else if( cmd == AAU ) {
        addr = 31;
    } else if( cmd == (LAD+addr) ) {
        status = LISTENER;
    } else if( inAddrRange(cmd, TAD) ) {
        if( cmd == (TAD + addr) )
            status = TALKER;
        else
            status = STAT_NONE;
    } else if( inAddrRange(cmd, AAD) ) {
        addr = cmd - AAD;
        rtn = cmd + 1;
    } else if( (cmd == SAI) && (status == TALKER) ) {
        rtn = 0x3E;
        sai = true;
    } else if( (cmd == SDI) && (status == TALKER) ) {
        rtn = 'J';
        sdi = devName;
    } else if( (cmd < DOE) && (status == LISTENER) ) {
        // Data
        fifo.push(cmd & 0xFF);
    } else if( sai ) {
        rtn = (cmd == 0x3E) ? ETO : ETE;
        sai = false;
    } else if( sdi ) {
        rtn = *devName++;
        if( rtn == 0 ) {
            rtn = ETO;
            sdi = NULL;
        }
    }
    return rtn;
}

#if 0
class Video(object):
    
    def __init__(self):
        self.video=Screen()
        self.address=31
        self.status="N"
        self.sai=False
        self.sdi=''
        self.q=deque((),2500)
        self.Scroll=0
        _thread.start_new_thread(self.handler,())

    def handler(self):
        while True:
            share.SDcheck(share.Tape[7:])
            share.TapeOK=share.TapeOK and share.SDOK[0]
            if share.Scroll:
                if self.Scroll!=0:
                    if self.Scroll>2:
                        self.video.store()
                    if self.Scroll==1:
                        self.video.down(False)
                    elif self.Scroll==2:
                        self.video.offset=len(self.video.lines)-self.video.ROWS
                        self.video.full()
                    elif self.Scroll==-1:
                        self.video.up(False,False)
                    share.display._write_reg(0x40,0x82)    
                    if self.Scroll<-1:
                        self.video.recall()
                        self.video.offset=0
                        self.video.set_cur()
                        share.Scroll=False
                    self.Scroll=0
            elif not(share.Menu):
                    if not len(self.q)==0:
                        try:
                            self.video.pr_char(self.q.popleft())
                        except Exception as e:
                            import sys
                            with open("error.log", "a") as f:
                                 sys.print_exception(e, f)
#endif