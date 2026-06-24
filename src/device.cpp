#include <stdio.h>
#include "hpil.h"

void CDevice::show(void)
{
    printf("%s: status:%c addr:%2d", devName, status==2?'T':((status==1)?'L':'-'), addr);
}

bool CDevice::base(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    printf("[");
    if( ((cmd == UNL) && (status == LISTENER))
            || ((cmd == UNT) && (status == TALKER)) ) {
        status = STAT_NONE;
        printf("1] ");
        return true;
    }
    if ((cmd == DCL) || ((cmd == SDC) && (status == LISTENER)) ) {
        clear();
        status = STAT_NONE;
        printf("2] ");
        return true;
    }
    if( cmd == AAU ) {
        addr = nAau;
        printf("3] ");
        return true;
    }
    if( cmd == (LAD+addr) ) {
        status = LISTENER;
        printf("4] ");
        return true;
    }
    if( inAddrRange(cmd, TAD) ) {
        if( cmd == (TAD + addr) )
            status = TALKER;
        else
            status = STAT_NONE;
        printf("5] ");
        return true;
    }
    if( inAddrRange(cmd, AAD) ) {
        addr = cmd - AAD;
        *rtn = cmd + 1;
        printf("6] ");
        return true;
    }
    if( sai ) {
        *rtn = (cmd == nSai) ? ETO : ETE;
        sai = false;
        printf("7] ");
        return true;
    }
    if( sdi ) {
        *rtn = *sdi++;
        if( *rtn == 0 ) {
            *rtn = ETO;
            sdi = NULL;
        }
        printf("8] ");
        return true;
    }

    printf("-] ");
    return false;
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