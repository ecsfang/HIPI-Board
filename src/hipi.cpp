#include <stdio.h>

#include "display.h"
#include "drive.h"


CDisplay    disp("TFDISPLAY", 0x3E, 31);
CDrive      drive("TFDRIVE", 0x10, 2);

void show(IL_CMD_t cmd = 0, IL_CMD_t rtn = 0)
{
    if( cmd != 0)
        printf("cmd: 0x%03X --> ", cmd);
    else
        printf("               ");
    disp.show();
    printf(" | ");
    drive.show();
    if( rtn != 0) {
        printf(" -> cmd: 0x%03X", rtn);
        if( rtn>='A' && rtn <= 'Z' )
            printf(" '%c'", rtn);
    }
    printf("\n");
}

IL_CMD_t loop(IL_CMD_t cmd)
{
    cmd = disp.hpil(cmd);
    cmd = drive.hpil(cmd);
    return cmd;
}

IL_CMD_t tests[] = {
    AAD+1,
    LAD+2,
    DDL+5,
    TAD+2,
    SST,
    0
};

void hipi(void)
{
    IL_CMD_t cmd, rtn;
    cmd = rtn = 0;
    show();
    for( int i=0; tests[i] != 0; i++ ) {
        cmd = tests[i];
        rtn = loop(cmd);
        show(cmd, rtn);
    }
    cmd = SST;
    do {
        rtn = loop(cmd);
        show(cmd, rtn);
    } while( rtn != ETO && rtn != DRV_IDLE && rtn != DRV_NO_TAPE_ERROR && rtn != DRV_NEW_TAPE_ERROR );

    printf("\nGet name ...\n");

    for( int n=1; n<3; n++ ) {
        cmd = TAD+n;
        rtn = loop(cmd);
        show(cmd, rtn);

        cmd = SDI;
        rtn = loop(cmd);
        show(cmd, rtn);
        cmd = SST;
        do {
            rtn = loop(cmd);
            show(cmd, rtn);
        } while( rtn != ETO && rtn != DRV_IDLE && rtn != DRV_NO_TAPE_ERROR && rtn != DRV_NEW_TAPE_ERROR );
        cmd = UNT;
        rtn = loop(cmd);
        show(cmd, rtn);
    }
    drive.close();
    printf("Done.\n");
}