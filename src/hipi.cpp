#include <stdio.h>

#include "display.h"
#include "drive.h"


CDisplay    disp("CDISPLAY", 0x3E, 31);
CDrive      drive("DRIVE", 0x10, 2);

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

int main(int argc, char *argv[])
{
    IL_CMD_t cmd, rtn;
    show();
    for( int i=0; tests[i] != 0; i++ ) {
        cmd = tests[i];
        rtn = loop(cmd);
        show(cmd, rtn);
    }
    cmd = SST;
    while( loop(SST) != DRV_IDLE ) {
        show(cmd, rtn);
    };
    drive.close();
    printf("Done.\n");
    return 0;
}