#include <stdio.h>

#include "display.h"
#include "drive.h"


CDisplay    disp("CDISPLAY", 0x3E, 31);
CDrive      drive("DRIVE", 0x10, 2);


int main(int argc, char *argv[])
{
    return 0;
}