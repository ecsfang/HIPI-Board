#include <stdio.h>
#include "drive.h"


Media_t mediaInfo[] = {
    { "HP82161A Cassette",            2, 1, 256 },
    { "HP9114B double sided disk",   77, 2,  16 },
    { "HDRIVE1 640K disk",           80, 2,  16 },
    { "HDRIVE1 2MB disk",           125, 1,  64 },
    { "HDRIVE1 4MB disk",           125, 2,  64 },
    { "HDRIVE1 8MB disk",           125, 4,  64 },
    { "HDRIVE1 16MB disk",          125, 8,  64 },
    { "unknown",                      0, 0,   0 }
};

static bool SDOK = false;

void disableSD() {
    SDOK = false;
    LOGF("\r\n * SD card access disabled");
}
void enableSD() {
    SDOK = true;
    LOGF("\r\n * SD card access enabled");
}
bool sdCardOK() {
    return SDOK;
}

int findMedia(unsigned int s)
{
    int i = 0;
    unsigned int sz;
    do {
        sz = mediaInfo[i].tracks * mediaInfo[i].surfaces * mediaInfo[i].blocks;
        if( sz == s )
            return i;
        if( sz == 0 )
            return i;
        i++;
    } while(sz);
    return -1;
}

// Power handling per the HP82161A manual (Loop Power Down, p. 10):
//   ON:      LPD acts as Device Clear + Interface Clear; power stays on.
//            (The real drive is also inaccessible for 0.7 s -- not emulated.)
//   (Both take effect on the RFC after LPD, which is still passed on.)
//   STANDBY: LPD powers the drive down (POWER light off). Any HP-IL signal
//            wakes it up again, as Device Clear + Interface Clear. Until it
//            has powered up (kPowerUpMs), frames are absorbed, not passed
//            on -- including the waking frame itself.
//   The manual also says the address becomes undefined in both cases, but
//   the HP-41 keeps using the old address without re-addressing the loop,
//   so the address is kept -- see powerUpReset().
IL_CMD_t CDrive::hpil(IL_CMD_t cmd)
{
    if( asleep_ ) {
        // First HP-IL signal after LPD: start powering up. A powered-down
        // device doesn't retransmit, so this frame -- and any others until
        // it's up -- is absorbed (the controller sees it not return).
        asleep_ = false;
        poweringUp_ = true;
        powerUpUntil_ = make_timeout_time_ms(kPowerUpMs);
        LOGF("\r\n * %s: woken up by HP-IL activity -- powering up", name());
        return IL_NO_FRAME;
    }
    if( poweringUp_ ) {
        servicePowerUp();
        if( poweringUp_ )
            return IL_NO_FRAME;
    }
    if( cmd == LPD ) {
        // Like other commands, LPD is only executed on the following Ready
        // For Command (manual: "RFC -- executes previous command"): the
        // controller waits for that RFC to come back, so the drive must
        // still pass it on before powering down (else the HP-41 reports
        // TIME OUT).
        lpdPending_ = true;
        return cmd;
    }
    if( lpdPending_ && cmd != RFC ) {
        lpdPending_ = false;                // another command came first
    }
    if( lpdPending_ && cmd == RFC ) {
        lpdPending_ = false;
        if( powerMode_ == DrivePower::Standby ) {
            if( tape->ok() )
                tape->close();              // flush and release the file
            setIdle();
            asleep_ = true;
            LOGF("\r\n * %s: Loop Power Down -- standby", name());
        } else {
            powerUpReset();
        }
        return cmd;                         // this RFC is still passed on
    }
    return CDevice::hpil(cmd);
}

void CDrive::clear(void)
{
    if( check() )
        tape->seek(0);
    mode = WRITE_MODE;
}

// Interface clear
void CDrive::ifc(void)
{
    status(STAT_IDLE);
    mode = WRITE_MODE;
    ddl = 31;
    ddt = 31;
    sst = DRV_IDLE;
}

// Do any pre-processing if needed
void CDrive::preProc(IL_CMD_t cmd)
{
    // Time of the last real HP-IL message frame -- see servicePendingDisable().
    // IDY frames (0x600-0x7FF) are left out: the controller keeps sending
    // them for service-request polling even when nothing is going on, so
    // counting them meant the loop never looked quiet.
    if( (cmd & 0x600) != 0x600 )
        lastFrame_ = get_absolute_time();
    if( ddl == 5 ) {
        //Busy formatting
        if( check() ) {
            LOGF("$$$ Formatting ... (%d)\r\n", tape->tell());
            markBusy();
            tape->write(buffer[0]);
            if( tape->tell() >= TAPE_SIZE ) {
                end = true;
                ddl = 31;
                sst = DRV_IDLE;
                tape->close();
                tape->open();
                LOGF("$$$ Done with formatting!\r\n");
            }
        }
    }
}

void CDrive::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn)
{
    if( cmd == NRD ) {
        end = true;
    } else {
        if( cmd == SDA ) {
            if( ddt > 4 )
                *rtn = ETO;
            else {
                end = false;
                *rtn = doNextTalker(cmd);
            }
        } else if( cmd == SST ) {
            check();
            *rtn = sst;
            end = true;
            // A status is reported once, then cleared: DRVTST reads 23 (New
            // Tape) after a power-up and expects the NEXT SST to say 0 --
            // anything else stops it with "STATUS nn". Conditions that
            // still hold (no tape, busy rewinding) are set again by check()
            // before the next SST.
            sst = DRV_IDLE;
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
                    m_tmp=0;
                }
            }
        } else if( IS_DATA(cmd) ) {
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
                // Rewind: the tape is wound back to its start. (On the real
                // drive it then can't be used until the door is opened and
                // closed, a Device Clear arrives or a Seek repositions it --
                // not emulated: the HP-41 always seeks before using it.)
                markBusy();         // tape moves: BUSY LED + spinning reels
                clear();
                break;
            case 8:
                //Close record
                writeblock();
                if( (mode == PARTIAL_MODE) && check() ) {
                    tape->seek(tape->tell()-BUF_SIZE);
                }
                break;
            case 9:
                // Transfer buffer 0 -> 1
                memcpy(buffer[1], buffer[0], BUF_SIZE);
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
                    // Write
                    pt = 0;
                    mode = WRITE_MODE;
                    break;
                case 4:
                    // Seek
                    m_tmp = 0;
                    mode = WRITE_MODE;
                    break;
                case 5:
                    // Format
                    LOGF("$$$ Do FORMAT ...\r\n");
                    clear();
                    sst = DRV_BUSY;
                    memset(buffer[0], 255, BUF_SIZE);
                    break;
                case 6:
                    // Partial write
                    readblock();
                    if( check() )
                        tape->seek(tape->tell()-BUF_SIZE);
                    mode = PARTIAL_MODE;
                    break;
                }
            }
        }
    } else if( IS_DATA(cmd) ) {
        *rtn = doNextListener(cmd);
    }
}

IL_CMD_t CDrive::doNextTalker(IL_CMD_t cmd) {
    IL_CMD_t rtn = cmd;
    if( ddt == 3 ) {
        // Send position (track+record+byte)
        if( check() ) {
            switch( m_tmp++ ) {
            case 0:
                // Return track number (0 or 1)
                rtn = (tape->tell() / BUF_SIZE) / REC_SIZE;
                break;
            case 1:
                // Return record number (0-255)
                rtn = (tape->tell() / BUF_SIZE) % REC_SIZE;
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
        // Send buffer 0 or 1
        rtn = buffer[ddt == 1 ? 1 : 0][pt];
        pt = (pt+1) % BUF_SIZE;
        if( pt == 0 ) {
            // Wrap buffer ...
            if( ddt == 1 ) {
                // Done with sending buffer 1
                end = true;
                ddt = MAX_CMD;
            } else {
                // Sending Buffer 0 or Read - so read next block
                if( check() ) {
                    if( tape->tell() >= TAPE_SIZE ) {
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
        buffer[1][pt] = cmd & 0xFF;
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
        if( m_tmp == 0 ) {
            // First byte is track number (0 or 1) -> m_tmp = 1|2
            m_tmp = 1+(cmd % BUF_SIZE);
        } else {
            // Second byte is record number (0-255)
            if( check() ) {
                unsigned int b = BUF_SIZE*(m_tmp-1)+(cmd % BUF_SIZE);
                if( b < size() ) {
                    markBusy();     // seek to record: BUSY LED + spinning reels
                    tape->seek(BUF_SIZE*b);
                    sst = DRV_IDLE;
                } else {
                    sst = DRV_SIZE_ERROR;
                }
            }
            m_tmp = 0;    // Done ...
            ddl = 31;
        }
        break;
    default: // DDL = 0, 2, ...
        // Add data to buffer 0 and write to tape if buffer full or EOI
        buffer[0][pt] = cmd & 0xFF;
        pt = (pt+1) % BUF_SIZE;
        if( (cmd & 0x200) == 0x200 || pt == 0 ) {
            writeblock();
            if( (mode == PARTIAL_MODE) && check() ) {
                if( pt == 0 ) {
                    readblock();
                }
                tape->seek(tape->tell()-BUF_SIZE);
            }
        }
    }
    return rtn;
}

void CDrive::readblock()
{
    if( check() ) {
        // Read BUF_SIZE bytes from tape
        LOGF("$$$ Read block ...\r\n");
        markBusy();
        tape->read(buffer[0]);
    }
}
void CDrive::writeblock()
{
    if( check() ) {
        // Write buffer[0] to tape
        LOGF("$$$ Write block ...\r\n");
        markBusy();
        tape->write(buffer[0]);
        // ... and flush!
        // tape.flush();
    }
}

bool CDrive::check()
{
    if( ejected_ ) {
        // Lid open (file picker showing) -- no tape, whatever is selected
        if( tape->ok() )
            tape->close();
        sst = DRV_NO_TAPE_ERROR;
        return false;
    }
    if( isRewinding() ) {
        // REWIND button in progress -- "executing a previous command"
        sst = DRV_BUSY;
        return false;
    }
    if( !tape->ok() ) {
        if( sdCardOK() ) {
            if( tape->media() && *tape->media() ) {
                LOGF("$$$ Opening tape file: %s ", tape->media());
                tud_cdc_n_write_flush(0);
                tud_task();
                markBusy();
                tape->open();
                size(tape->mediaSize());
                int i = findMedia(size());
                LOGF("media: %s ", mediaInfo[i].media);
                if( size() <= 0 )
                    size(512);
                LOGF("size=%dkb\r\n", (size()*BUF_SIZE)/1024);
                tape->seek(0);
                sst = DRV_NEW_TAPE_ERROR;
            } else {
                LOGF("$$$ No tape selected!\r\n");
                tud_cdc_n_write_flush(0);
                tud_task();
                sst = DRV_NO_TAPE_ERROR;
            }
            //TapeOK = true;
        } else {
            LOGF("$$$ No tape file!\r\n");
            tud_cdc_n_write_flush(0);
            tud_task();
            sst = DRV_NO_TAPE_ERROR;
        }
    }
    return tape->ok();
}

void CDrive::show()
{
    CDevice::show();
    char buf[16];
    char sBuf[256];
    int n=0;
    n += sprintf(sBuf+n, "\r\n\tmode:%s", mode == WRITE_MODE ? "WRITE" : "PARTIAL");
    n += sprintf(sBuf+n, " ddl:%d ddt:%d sst:%d last:%s", ddl, ddt, sst, ilMnemonic(last, buf));
    LOGF(sBuf);
    LOGF("\r\n\ttape:'%s' size:%d", (void*)tape->media(), size());
}

static void list_dir(const char* path, int depth) {
    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        LOGF("$$$ %*s[opendir %s: %s]\r\n", depth*2, "", path, FRESULT_str(fr));
        return;
    }

    while (true) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;   // 0 = end of directory

        const char* kind = (fno.fattrib & AM_DIR) ? "DIR " : "FILE";
        LOGF("$$$ \t%*s%-4s %8lu  %s\r\n",
               depth*2, "",
               kind,
               (unsigned long)fno.fsize,
               fno.fname);

        // Recurse into subdirectories (but not "." and "..")
        if (fno.fattrib & AM_DIR) {
            char subpath[520];     // path (<= 255) + "/" + name (<= 255) + NUL
            if (snprintf(subpath, sizeof(subpath), "%s/%s", path, fno.fname) >= static_cast<int>(sizeof(subpath)))
                continue;          // path too long -- skip it
            list_dir(subpath, depth + 1);
        }
    }
    f_closedir(&dir);
}

void sd_dir() {
    LOGF("\r\n\t=== SD card contents ===\r\n");
    list_dir("", 0);
    LOGF("\t=== done ===\r\n");
}

