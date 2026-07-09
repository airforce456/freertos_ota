#include "bsp_init.h"
#include "bsp_i2c.h"

void BSP_Init(void)
{
    MyI2C_Init();  /* 接管 PB6/PB7，释放硬件 I2C1 */
}
