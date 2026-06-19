#include <stdio.h>
#include "drive.h"

static char *devName = "CDRIVE";

IL_CMD_t CDrive::hpil(IL_CMD_t cmd)
{
    IL_CMD_t  rtn = cmd;

    if( ddl == 5 ) {
        //Busy formatting
        //if self.check():
        //    self.tape.write(self.buf0)
        //    if self.tape.tell()>=131072:
        //        self.end=True
        //        self.ddl=31
        //        self.sst=0
        //        self.tape.close()
        //        self.tape=open(share.Tape,'+b')
    }
    if( cmd == IFC) {
        status = STAT_NONE;
        mode = MODE_NONE;
        ddl=31;
        ddt=31;
        sst=0;
    } else if( (cmd == DCL) || ((cmd == SDC) && (status == LISTENER)) ) {
        //if self.check():
        //    self.tape.seek(0)
        status = STAT_NONE;
        mode = MODE_NONE;
    } else if( cmd == AAU ) {
        addr = 2;
    } else if( cmd == (LAD+addr) ) {
        status = LISTENER;
    } else if( inAddrRange(cmd, TAD) ) {
        if( cmd == (TAD + addr) ) {
            status = TALKER;
            end = true;
        } else {
            status = STAT_NONE;
        }
    } else if( inAddrRange(cmd, AAD) ) {
        addr = cmd - AAD;
        rtn = cmd+1;
    } else if( status == TALKER ) {
        if( cmd == UNT ) {
            status = STAT_NONE;
        } else if( cmd == NRD ) {
            end = true;
        } else {
            if( cmd == SDA ) {
                if( ddt > 4 )
                    rtn = 0x540;
                else {
                    end = false;
                    rtn = next(cmd);
                }
            } else if( cmd == SAI ) {
                rtn = 0x10;
                end = true;
            } else if( cmd == SST ) {
                //self.check()
                //y=self.sst
                end = true;
            } else if( cmd == SDI ) {
                rtn = 'J';
                sdi = devName;
                end = false;
            } else if( inAddrRange(cmd, DDT) ) {
                IL_ADDR_t n = cmd & MAX_ADDR;
                if( n == 4 ) {
                    //Exchange buffers
                    //for i in range(256):
                    //    u=self.buf0[i]
                    //    self.buf0[i]=self.buf1[i]
                    //    self.buf1[i]=u
                    //self.pt=0
                } else {
                    ddt = n;
                    end = false;
                    switch (n) {
                    case 3:
                        // Send Position
                        tmp=0;
                        break;
                    case 2:
                        // Read
                        //self.readblock()
                        //self.pt=0
                        mode = MODE_NONE;
                        break;
                    case 0:
                        // Send Buffer 0
                        mode = MODE_NONE;
                    }
                }
            } else if( cmd < DOE ) {
                // Data
                if( cmd != last )
                    rtn = 0x541;
                else {
                    if( end )
                        rtn = 0x540;
                    else
                        rtn = next(cmd);
                }
            }
            last = rtn;
        }
    } else if( status == LISTENER ) {
        if( inAddrRange(cmd, DDL) ) {
            IL_ADDR_t n = cmd & MAX_ADDR;
            if( n > 10 ) {
                mode = MODE_NONE;
            } else {
                switch( n ) {
                case 10:
                    //Exchange buffers
                    //for i in range(256):
                    //    u=self.buf0[i]
                    //    self.buf0[i]=self.buf1[i]
                    //    self.buf1[i]=u
                    //self.pt=0
                    break;
                case 9:
                    // Transfer buffer
                    //for i in range(256):
                    //    self.buf1[i]=self.buf0[i]
                    //self.pt=0
                    break;
                case 8:
                    //Close record
                    //self.writeblock()
                    //if (self.mode=="P") and self.check():
                    //    self.tape.seek(self.tape.tell()-256)
                    break;
                case 7:
                    //Rewind (incomplete)
                    //if self.check():
                    //    self.tape.seek(0)
                    break;
                default:
                    ddl = n;
                    switch( n ) {
                    case 5:
                        //Format
                        //mode = MODE_NONE;
                        //self.sst=32
                        //for i in range(256):
                        //    self.buf0[i]=255
                        break;
                    case 6:
                        //Partial write
                        //self.readblock()
                        //if self.check():
                        //    self.tape.seek(self.tape.tell()-256)
                        mode = P_MODE;
                        break;
                    case 4:
                        //Seek
                        //self.tmp=0
                        mode = MODE_NONE;
                        break;
                    case 2:
                        //Write
                        //self.pt=0
                        mode = MODE_NONE;
                        break;
                    }
                }
            }
        } else if( cmd == UNL ) {
            status = STAT_NONE;
        } else if( cmd < DOE ) {
            rtn = next(cmd);
        }
    }
    return rtn;
}
