#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NPAGE  5000       // 테스트용 페이지 개수
#define PSIZE  4096

char *pages[NPAGE];

int
main(void)
{
  int i, j;

  printf("testswap: allocate & write pattern\n");

  // 1) 페이지 할당 + 패턴 쓰기
  for (i = 0; i < NPAGE; i++) {
    char *p = sbrk(PSIZE);
    if (p == (char*)-1) {
      printf("testswap: sbrk failed at %d\n", i);
      exit(1);
    }
    pages[i] = p;

    for (j = 0; j < PSIZE; j++) {
      p[j] = (char)((i ^ j) & 0xff);
    }

    if (i % 100 == 0)
      printf("testswap: filled %d pages\n", i);
  }

  // 2) 여기서 memhog를 자식 프로세스로 실행
  int pid = fork();
  if (pid < 0) {
    printf("testswap: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // 자식: memhog 실행
    char *argv[] = { "memhog", 0 };
    printf("testswap(child): exec memhog...\n");
    exec("memhog", argv);
    printf("testswap(child): exec memhog failed\n");
    exit(1);
  }

  // 부모: swapstat 기준값 저장
  int r0 = 0, w0 = 0;
  swapstat(&r0, &w0);

  printf("testswap(parent): memhog pid=%d, sleep to let it swap...\n", pid);

  // 3) memhog가 메모리 빡세게 쓰도록 충분히 기다리기
  sleep(300);   // 필요하면 더 늘려도 됨

  // 4) swapstat 변화 확인
  int r1 = 0, w1 = 0;
  swapstat(&r1, &w1);
  printf("testswap(parent): swapstat diff: read +%d write +%d\n",
         r1 - r0, w1 - w0);

  // 5) 패턴 검증
  printf("testswap(parent): verify pattern\n");

  for (i = 0; i < NPAGE; i++) {
    char *p = pages[i];
    for (j = 0; j < PSIZE; j++) {
      char expect = (char)((i ^ j) & 0xff);
      if (p[j] != expect) {
        printf("testswap: CORRUPTED at page %d, offset %d (got %d, expect %d)\n",
               i, j, (uchar)p[j], (uchar)expect);
        // memhog 자식 정리하고 종료
        kill(pid);
        wait(0);
        exit(1);
      }
    }
    if (i % 100 == 0)
      printf("testswap: verified %d pages\n", i);
  }

  printf("testswap: ALL OK, no corruption\n");

  // 6) memhog 자식 프로세스 종료시키고 wait
  kill(pid);
  wait(0);

  exit(0);
}

