#include <stdio.h>
#include <cstring>
#include "drive.h"

void CDrive::clear(void)
{
    if( check() )
        tape.seek(0);
    mode = WRITE_MODE;
}

IL_CMD_t CDrive::hpil(IL_CMD_t cmd)
{
    IL_CMD_t rtn = cmd;

    if( ddl == 5 ) {
        //Busy formatting
        printf("Formatting ... (%d)\n", tape.tell());
        if( check() ) {
            tape.write(buf0);
            if( tape.tell() >= TAPE_SIZE ) {
                end = true;
                ddl = 31;
                sst = DRV_IDLE;
                tape.close();
                tape.open();
            }
        }
    }

    // Handle all base commands
    if( base(cmd, &rtn) )
        return rtn;

    // Otherwise handle device specific commands
    if( cmd == IFC) {
        status = STAT_NONE;
        mode = WRITE_MODE;
        ddl = 31;
        ddt = 31;
        sst = DRV_IDLE;
    } else if( status == TALKER ) {
        doTalker(cmd, &rtn);
    } else if( status == LISTENER ) {
        doListener(cmd, &rtn);
    }
    return rtn;
}

void CDrive::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    if( cmd == UNT ) {
        status = STAT_NONE;
    } else if( cmd == NRD ) {
        end = true;
    } else {
        if( cmd == SDA ) {
            if( ddt > 4 )
                *rtn = ETO;
            else {
                end = false;
                *rtn = doNextTalker(cmd);
            }
        } else if( cmd == SAI ) {
            *rtn = nSai;
            end = true;
        } else if( cmd == SST ) {
            check();
            *rtn = sst;
            end = true;
        } else if( cmd == SDI ) {
            *rtn = 'J';
            sdi = devName;
            end = false;
        } else if( inAddrRange(cmd, DDT) ) {
            IL_ADDR_t n = cmd & MAX_ADDR;
            if( n == 4 ) {
                //Exchange buffers
                exchangeBuf();
                pt = 0;
            } else {
                ddt = n;
                end = false;
                switch (n) {
                case 0:
                    // Send Buffer 0
                    mode = WRITE_MODE;
                    break;
                case 2:
                    // Read
                    readblock();
                    pt = 0;
                    mode = WRITE_MODE;
                    break;
                case 3:
                    // Send Position
                    tmp=0;
                }
            }
        } else if( cmd < DOE ) {
            // Data
            if( cmd != last )
                *rtn = ETE;      // Error
            else {
                *rtn = end ? ETO : doNextTalker(cmd);
            }
        }
        last = *rtn;
    }
}

void CDrive::doListener(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    *rtn = cmd;
    if( inAddrRange(cmd, DDL) ) {
        IL_ADDR_t n = cmd & MAX_CMD;
        if( n > 10 ) {
            mode = WRITE_MODE;
        } else {
            switch( n ) {
            case 7:
                //Rewind (incomplete)
                if( check() )
                    tape.seek(0);
                break;
            case 8:
                //Close record
                writeblock();
                if( (mode == PARTIAL_MODE) && check() ) {
                    tape.seek(tape.tell()-BUF_SIZE);
                }
                break;
            case 9:
                // Transfer buffer
                memcpy(buf1, buf0, BUF_SIZE);
                pt = 0;
                break;
            case 10:
                //Exchange buffers
                exchangeBuf();
                pt = 0;
                break;
            default:    // DDL = 0-6
                ddl = n;
                switch( ddl ) {
                case 2:
                    //Write
                    pt = 0;
                    mode = WRITE_MODE;
                    break;
                case 4:
                    //Seek
                    tmp = 0;
                    mode = WRITE_MODE;
                    break;
                case 5:
                    // Format
                    printf("Do FORMAT ...\n");
                    mode = WRITE_MODE;
                    sst = DRV_BUSY;
                    memset(buf0, 255, BUF_SIZE);
                    break;
                case 6:
                    //Partial write
                    readblock();
                    if( check() )
                        tape.seek(tape.tell()-BUF_SIZE);
                    mode = PARTIAL_MODE;
                    break;
                }
            }
        }
    } else if( cmd == UNL ) {
        status = STAT_NONE;
    } else if( cmd < DOE ) {
        if( status == LISTENER )
            *rtn = doNextListener(cmd);
        else if( status == TALKER )
            *rtn = doNextTalker(cmd);
    }
}

IL_CMD_t CDrive::doNextTalker(IL_CMD_t cmd) {
    IL_CMD_t rtn = cmd;
    if( sdi ) {
        // We are sending the device name character by character
        rtn = (IL_CMD_t)*sdi++;
        if( !*sdi ) {
            // Done sending device name
            end = true;
            sdi = NULL;
        }
    } else if( ddt == 3 ) {
        // Send position
        if( check() ) {
            switch( tmp++ ) {
            case 0:
                // Return track number (0 or 1)
                rtn = (tape.tell() / BUF_SIZE) / REC_SIZE;
                break;
            case 1:
                // Return record number (0-255)
                rtn = (tape.tell() / BUF_SIZE) % REC_SIZE;
                break;
            case 2:
                // Return byte pointer (0-255)
                rtn = pt;
                ddt = MAX_CMD;
                end = true;
            }
        } else {
            rtn = ETE;  // Error
            end = true;
            ddt = MAX_CMD;
        }
    } else if( ddt < 3 ) {
        if( ddt == 1 ) {
            // Send buffer 1
            rtn = buf1[pt];
        } else {
            // send Buffer 0 or Read
            rtn = buf0[pt];
        }
        pt = (pt+1) % BUF_SIZE;
        if( pt == 0 ) {
            if( ddt == 1 ) {
                // Done with sending buffer 1
                end = true;
                ddt = MAX_CMD;
            } else {
                // Sending Buffer 0 or Read - so read nect block
                if( check() ) {
                    if( tape.tell() >= size*BUF_SIZE ) {
                        end = true;
                        ddt = MAX_CMD;
                    } else {
                        readblock();
                    }
                }
            }
        }
    }
    return rtn;
}

IL_CMD_t CDrive::doNextListener(IL_CMD_t cmd) {
    IL_CMD_t rtn = cmd;
    switch(ddl) {
    case 1:
        // Write buffer 1
        buf1[pt] = cmd & 0xFF;
        pt = (pt+1) % BUF_SIZE;
        break;
    case 3:
        // Set byte pointer
        pt = cmd % BUF_SIZE;
        break;
    case 4:
        // Seek
        // The next two Data Bytes are interpreted as a track number (O or 1)
        // and a record number (O thru 255), and the tape is positioned to
        // that track and record. If the track number is greater than 1,
        // the tape is not moved and a Size Error is generated (refer to
        // the Status Byte Definition table above). (This command also clears
        // the partial recording mode set up by the Partial Write command —
        // Device Dependent Listener 6 command.)
        if( tmp == 0 ) {
            // First byte is track number (0 or 1)
            tmp = 1+(cmd % BUF_SIZE);
        } else {
            // Second byte is record number (0-255)
            if( check() ) {
                unsigned int b = BUF_SIZE*(tmp-1)+(cmd % BUF_SIZE);
                if( b < size ) {
                    tape.seek(BUF_SIZE*b);
                    sst = DRV_IDLE;
                } else {
                    sst = DRV_SIZE_ERROR;
                }
            }
            tmp = 0;    // Done ...
            ddl = 31;
        }
        break;
    default: // DDL = 0, 2, ...
        // Add data to buffer 0 and write to tape if buffer full or EOI
        buf0[pt] = cmd & 0xFF;
        pt = (pt+1) % BUF_SIZE;
        if( (cmd & 0x200) == 0x200 || pt == 0 ) {
            writeblock();
            if( (mode == PARTIAL_MODE) && check() ) {
                if( pt == 0 ) {
                    readblock();
                }
                tape.seek(tape.tell()-BUF_SIZE);
            }
        }
    }
    return rtn;
}

void CDrive::readblock()
{
    if( check() ) {
        // Read BUF_SIZE bytes from tape
        tape.read(buf0);
    }
}
void CDrive::writeblock()
{
    if( check() ) {
        // Write buf0 to tape
        tape.write(buf0);
        // ... and flush!
        // tape.flush();
    }
}
bool TapeOK = false;
bool SDOK = true;
const char *share_Tape = "tape.bin";

bool CDrive::check()
{
    if( !TapeOK ) {
        if( SDOK ) {
            printf("Opening tape file: %s ", share_Tape);
            tape.open(share_Tape);
            tape.seek(24);
            size=tape.readInt()*tape.readInt()*tape.readInt();
            if( size == 0 )
                size = 512;
            printf("size=%ld\n", size);
            tape.seek(0);
            sst = DRV_NEW_TAPE_ERROR;
            TapeOK = true;
        } else {
            sst = DRV_NO_TAPE_ERROR;
        }
    }
    return TapeOK;
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