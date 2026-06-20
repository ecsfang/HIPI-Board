#include <stdio.h>
#include <cstring>
#include "drive.h"

void CDrive::clear(void)
{
    //if self.check():
    //    self.tape.seek(0)
    mode = MODE_NONE;
}

IL_CMD_t CDrive::hpil(IL_CMD_t cmd)
{
    IL_CMD_t rtn = cmd;

    if( ddl == 5 ) {
        //Busy formatting
        if( check() ) {
            tape.write(buf0);
            if( tape.tell() >= 131072 ) {
                end = true;
                ddl = 31;
                sst = 0;
                tape.close();
                //tape = open(share.Tape,'+b');
            }
        }
    }

    if( base(cmd, &rtn) )
        return rtn;

    if( cmd == IFC) {
        status = STAT_NONE;
        mode = MODE_NONE;
        ddl=31;
        ddt=31;
        sst=0;
    } else if( status == TALKER ) {
        if( cmd == UNT ) {
            status = STAT_NONE;
        } else if( cmd == NRD ) {
            end = true;
        } else {
            if( cmd == SDA ) {
                if( ddt > 4 )
                    rtn = ETO;
                else {
                    end = false;
                    rtn = next(cmd);
                }
            } else if( cmd == SAI ) {
                rtn = nSai;
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
                    rtn = ETE;      // Error
                else {
                    if( end )
                        rtn = ETO;  // OK!
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
                        mode = MODE_NONE;
                        sst = 32;
                        memset(buf0, 255, 256);
                        break;
                    case 6:
                        //Partial write
                        readblock();
                        if( check() )
                            tape.seek(tape.tell()-256);
                        mode = P_MODE;
                        break;
                    case 4:
                        //Seek
                        tmp = 0;
                        mode = MODE_NONE;
                        break;
                    case 2:
                        //Write
                        pt = 0;
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

IL_CMD_t CDrive::next(IL_CMD_t cmd) {
    IL_CMD_t rtn = cmd;
    if( status == TALKER ) {
        if( sdi ) {
            rtn = (IL_CMD_t)*sdi;
            sdi++;
            if( !*sdi ) {
                end = true;
                sdi = NULL;
            } else if( ddt == 3 ) {
                // Send position
                if( check() ) {
                    if( tmp == 0 ) {
                        rtn = (tape.tell() / 256) / 256;
                    } else if( tmp == 1 ) {
                        rtn = (tape.tell() / 256) % 256;
                    } else {
                        rtn = pt;
                        ddt = 31;
                        end = true;
                    }
                    tmp++;
                } else {
                    rtn = 0x541;
                    ddt = 31;
                }
            } else if( ddt < 3 ) {
                if( ddt == 1 ) {
                    // Send buffer 1
                    rtn = buf1[pt];
                } else {
                    // send Buffer 0 or Read
                    rtn = buf0[pt];
                }
                pt = (pt+1) % 256;
                if( pt == 0 ) {
                    if( ddt == 1 ) {
                        // Send buffer 1
                        end = true;
                        ddt = 31;
                    } else {
                        // send Buffer 0 or Read
                        if( check() ) {
                            if( tape.tell() >= size*256 ) {
                                end = true;
                                ddt = 31;
                            } else {
                                readblock();
                            }
                        }
                    }
                }
            }
        }
    } else if( status == LISTENER ) {
        if( ddl == 3 ) {
            // Set byte pointer
            pt = cmd % 256;
        } else if( ddl == 4 ) {
            // Seek
            if( tmp == 0 ) {
                tmp = 1+(cmd % 256);
            } else {
                if( check() ) {
                    unsigned int b = 256*(tmp-1)+(cmd % 256);
                    if( b < size ) {
                        tape.seek(256*b);
                        sst = 0;
                    } else {
                        sst = 0x1c;
                    }
                }
                tmp = 0;
                ddl = 31;
            }
        } else if( ddl == 1 ) {
            // Write buffer 1
            buf1[pt] = cmd % 256;
            pt = (pt+1) % 256;
        } else {
            buf0[pt] = cmd % 256;
            pt = (pt+1) % 256;
            if( (cmd & 0x200) != 0 ) {
                writeblock();
                if( (mode == P_MODE) && check() ) {
                    if( pt == 0 ) {
                        readblock();
                    }
                    tape.seek(tape.tell()-256);
                }
            } else if( pt == 0 ) {
                writeblock();
                if( (mode == P_MODE) && check() ) {
                    readblock();
                    tape.seek(tape.tell()-256);
                }
            }
        }
    }
    return rtn;
}
void CDrive::readblock()
{
    if( check() ) {
        // Read 256 bytes from tape
        // If less than 256 fill with 256 ...
    }
}
void CDrive::writeblock()
{
    if( check() ) {
        // Write buf0 to tape
        // ... and flush!
    }
}
bool CDrive::check()
{
    return false;
}
#if 0
    def readblock(self):
        if self.check():
            buf=bytearray(self.tape.read(256))
            self.buf0[:len(buf)]=buf
            for i in range(len(buf),256):
                self.buf0[i]=255
        

    def writeblock(self):
        if self.check():
            self.tape.write(self.buf0)
            self.tape.flush()
        
    def check(self):
        if share.TapeOK:
            return True
        else:
            if share.SDOK[0]:
                self.tape=open(share.Tape,'+b')
                self.tape.seek(24)
                self.size=int.from_bytes(self.tape.read(4))*int.from_bytes(self.tape.read(4))*int.from_bytes(self.tape.read(4))
                if self.size==0:
                    self.size=512
                self.tape.seek(0)                
                self.sst=0x17
                share.TapeOK=True
                return True
            else:
                self.sst=0x14
                return False
#endif