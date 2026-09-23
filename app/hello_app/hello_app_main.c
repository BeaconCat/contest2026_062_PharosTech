/****************************************************************************
 * Contest 2026 team 000 - hello app sample
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/power/pm.h>

extern int kickpi_k7_wifi_prepare_sleep(void);
extern int kickpi_k7_wifi_abort_sleep(void);

int main(int argc, char *argv[])
{
  int prepare;
  int restore;
  int sleep;

  if (argc > 1 && strcmp(argv[1], "pmtest") == 0)
    {
      prepare = kickpi_k7_wifi_prepare_sleep();
      printf("wifi prepare: %d\n", prepare);
      if (prepare < 0)
        {
          return 1;
        }

      sleep = pm_changestate(PM_IDLE_DOMAIN, PM_SLEEP);
      printf("pm sleep: %d\n", sleep);
      if (sleep < 0)
        {
          printf("wifi abort: %d\n", kickpi_k7_wifi_abort_sleep());
          return 1;
        }

      usleep(500000);
      restore = pm_changestate(PM_IDLE_DOMAIN, PM_RESTORE);
      printf("pm restore: %d\n", restore);
      usleep(1000000);
      return restore < 0 ? 1 : 0;
    }

  printf("Hello from openvela contest 2026 team 000!\n");
  return 0;
}
