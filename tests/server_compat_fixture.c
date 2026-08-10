#include <windows.h>
#include <cfgmgr32.h>

void mainCRTStartup(void)
{
    DEVINST child = 0xdeadbeef;
    CONFIGRET child_result = CM_Get_Child(&child, 0, 0);
    ExitProcess(child_result == CR_NO_SUCH_DEVNODE && child == 0 ? 0 : 1);
}
