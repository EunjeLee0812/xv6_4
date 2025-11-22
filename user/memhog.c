// user/memhog.c

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXPAGES  30000
#define PGSIZE    4096

static char *pages[MAXPAGES];

int
main(void)
{
  int n = 0;
  int last_write = 0;

  printf("memhog: start\n");

  // 1. 페이지를 하나씩 늘리면서,
  //    100페이지마다 swapstat도 같이 찍어보자.
  for (n = 0; n < MAXPAGES; n++) {
    char *p = sbrk(PGSIZE);
    if (p == (char*)-1) {
      printf("memhog: sbrk failed at %d pages\n", n);
      break;
    }

    p[0] = 1;       // 실제로 한 번 건드려서 물리 페이지 매핑
    pages[n] = p;

    if (n % 100 == 0) {
      int nr_read = 0, nr_write = 0;
      swapstat(&nr_read, &nr_write);
      printf("memhog: allocated %d pages, swapstat: read=%d write=%d\n",
             n, nr_read, nr_write);

      // swapout이 실제로 시작되면, 할당 루프를 여기서 끊고
      // 아래 while(1) 루프로 내려갈 수도 있음.
      if (nr_write > last_write) {
        printf("memhog: swap started (write counter increased), break alloc loop\n");
        last_write = nr_write;
        n++;  // 현재까지 확보한 페이지 개수 유지
        break;
      }
      last_write = nr_write;
    }
  }

  printf("memhog: touch & stat loop start, n=%d\n", n);

  // 2. 이제 이미 많이 할당한 상태에서,
  //    주기적으로 오래된 페이지들을 건드려서 swapin 유도 + swapstat 찍기
  while (1) {
    for (int i = 0; i < n; i += 97) {
      pages[i][0]++;   // swapin 유도하는 access
    }

    int nr_read = 0, nr_write = 0;
    swapstat(&nr_read, &nr_write);
    printf("swapstat: read=%d write=%d\n", nr_read, nr_write);

    sleep(10);
  }

  exit(0);
}

