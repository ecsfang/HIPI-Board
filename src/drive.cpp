#include <stdio.h>
#include <cstring>
#include "drive.h"
#include "usb_serial.h"

void CDrive::clear(void)
{
    if( check() )
        tape.seek(0);
    mode = WRITE_MODE;
}

IL_CMD_t CDrive::hpil(IL_CMD_t cmd)
{
    IL_CMD_t rtn = cmd;

    if( bPrt ) cdc0_printf("DRV:%3X ", cmd);

    if( ddl == 5 ) {
        //Busy formatting
#if 1
        cdc0_printf("Formatting ... (%d)\r\n", tape.tell());
        if( check() ) {
            tape.write(buf0);
            if( tape.tell() >= TAPE_SIZE ) {
                end = true;
                ddl = 31;
                sst = DRV_IDLE;
                tape.close();
                tape.open();
                cdc0_printf("Done with formatting!\r\n");
            }
        }
#else
        cdc0_printf("Formatting ... (xxx)\r\n");
        end = true;
        ddl = 31;
        sst = DRV_IDLE;
#endif
    }

    // Handle all base commands
    if( base(cmd, &rtn) ) {
        return rtn;
    }

    // Otherwise handle device specific commands
    if( cmd == IFC) {
        status(STAT_IDLE);
        mode = WRITE_MODE;
        ddl = 31;
        ddt = 31;
        sst = DRV_IDLE;
    } else if( isTalker() ) {
        doTalker(cmd, &rtn);
    } else if( isListener() ) {
        doListener(cmd, &rtn);
    }
    return rtn;
}

void CDrive::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    if( cmd == UNT ) {
        status(STAT_IDLE);
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
            sai = true;
        } else if( cmd == SST ) {
            check();
            *rtn = sst;
            end = true;
        } else if( cmd == SDI ) {
            sdi = devName;
            *rtn = *sdi++;
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
                    cdc0_printf("Do FORMAT ...\r\n");
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
        status(STAT_IDLE);
    } else if( cmd < DOE ) {
        if( isListener() )
            *rtn = doNextListener(cmd);
        else if( isTalker() )
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
        cdc0_printf("Read block ...\r\n");
        tape.read(buf0);
    }
}
void CDrive::writeblock()
{
    if( check() ) {
        // Write buf0 to tape
        cdc0_printf("Write block ...\r\n");
        tape.write(buf0);
        // ... and flush!
        // tape.flush();
    }
}
bool TapeOK = false;
bool SDOK = false;
const char *share_Tape = "tape.bin";

bool CDrive::check()
{
    if( !TapeOK ) {
        if( SDOK ) {
            cdc0_printf("Opening tape file: %s ", share_Tape);
            tape.open(share_Tape);
            tape.seek(24);
            size=tape.readInt()*tape.readInt()*tape.readInt();
            if( size == 0 )
                size = 512;
            cdc0_printf("size=%d\r\n", size);
            tape.seek(0);
            sst = DRV_NEW_TAPE_ERROR;
            TapeOK = true;
        } else {
            cdc0_printf("No tape file!\r\n");
            sst = DRV_NO_TAPE_ERROR;
        }
    }
    return TapeOK;
}


#include <stdio.h>
#include "ff.h"

static FATFS fs;

static void list_dir(const char* path, int depth) {
    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        cdc0_printf("%*s[opendir %s: %s]\r\n", depth*2, "", path, FRESULT_str(fr));
        return;
    }

    while (true) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;   // 0 = slut på katalogen

        const char* kind = (fno.fattrib & AM_DIR) ? "DIR " : "FILE";
        cdc0_printf("\t%*s%-4s %8lu  %s\r\n",
               depth*2, "",
               kind,
               (unsigned long)fno.fsize,
               fno.fname);

        // Rekursera ned i underkataloger (men inte "." och "..")
        if (fno.fattrib & AM_DIR) {
            char subpath[256];
            snprintf(subpath, sizeof(subpath), "%s/%s", path, fno.fname);
            list_dir(subpath, depth + 1);
        }
    }
    f_closedir(&dir);
}

void sd_dir() {
    cdc0_printf("\r\n\t=== SD card contents ===\r\n");
    list_dir("", 0);
    cdc0_printf("\t=== done ===\r\n");
}

