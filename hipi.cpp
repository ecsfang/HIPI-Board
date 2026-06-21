#include <stdio.h>

#include "display.h"
#include "drive.h"


CDisplay    disp("CDISPLAY", 0x3E, 31);
CDrive      drive("DRIVE", 0x10, 2);

void show(void)
{
    disp.show();
    printf(" | ");
    drive.show();
    printf("\n");
}

IL_CMD_t loop(IL_CMD_t cmd)
{
    cmd = disp.hpil(cmd);
    cmd = drive.hpil(cmd);
    return cmd;
}
int main(int argc, char *argv[])
{
    show();
    loop(AAD+1);
    show();
    loop(TAD+2);
    show();
    return 0;
}