// user/forkswap.c
// 목적:
// 1) 부모가 많은 페이지에 패턴을 써놓고
// 2) fork()로 자식을 만든 다음,
// 3) 자식은 "짝수 페이지"만 다른 패턴으로 덮어쓰고 계속 살아 있고
// 4) 부모는 memhog를 돌려서 swap을 왕창 일으킨 뒤
// 5) "부모 자신의 페이지 내용이 그대로인지"를 검사하는 테스트.
//
// 기대 결과:
// - 부모 쪽 메모리는 자식이 건드려도 절대 바뀌면 안 됨 (별도 주소 공간이니까)
// - swap이 많이 일어나도 데이터가 깨지지 않아야 함

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NPAGE  2000      // 테스트할 페이지 개수 (필요하면 3000~5000으로 늘려도 됨)
#define PSIZE  4096      // 페이지 크기 (PGSIZE)

// 부모가 쓰는 기본 패턴: (i ^ j) & 0xff
static inline uchar
parent_pattern(int i, int j)
{
  return (uchar)((i ^ j) & 0xff);
}

// 자식이 짝수 페이지에 덮어쓸 패턴: 부모 패턴의 bit 반전(~)
static inline uchar
child_pattern(int i, int j)
{
  return (uchar)(~((i ^ j) & 0xff));
}

static char *pages[NPAGE];

int
main(void)
{
  int i, j;

  printf("forkswap: allocate & write parent pattern\n");

  // 1) 부모가 NPAGE 페이지를 sbrk로 할당하고, 각 페이지에 부모 패턴을 써 넣음
  for(i = 0; i < NPAGE; i++){
    char *p = sbrk(PSIZE);
    if(p == (char *)-1){
      printf("forkswap: sbrk failed at %d\n", i);
      exit(1);
    }
    pages[i] = p;

    for(j = 0; j < PSIZE; j++){
      p[j] = parent_pattern(i, j);
    }

    if(i % 100 == 0)
      printf("forkswap: filled %d pages\n", i);
  }

  // 2) fork로 자식 생성
  int cpid = fork();
  if(cpid < 0){
    printf("forkswap: fork failed\n");
    exit(1);
  }

  if(cpid == 0){
    // === 자식 프로세스 ===
    printf("forkswap(child): modify even pages with child pattern\n");

    // 짝수 페이지(0,2,4,...)에만 다른 패턴을 덮어씀
    for(i = 0; i < NPAGE; i += 2){
      char *p = pages[i];
      for(j = 0; j < PSIZE; j++){
        p[j] = child_pattern(i, j);
      }
      if(i % 500 == 0)
        printf("forkswap(child): modified page %d\n", i);
    }

    printf("forkswap(child): done modifying, go to sleep to keep pages alive\n");
    // 부모가 memhog를 돌려서 swap을 많이 일으키는 동안
    // 이 자식 프로세스도 메모리를 쥐고 있게 일부러 안 죽고 잠만 잔다.
    while(1){
      sleep(100);
    }

    // 도달하지 않겠지만, 형식상
    exit(0);
  }

  // === 부모 프로세스 ===
  int r0 = 0, w0 = 0;
  swapstat(&r0, &w0);
  printf("forkswap(parent): before memhog, swapstat: read=%d write=%d\n", r0, w0);

  // 3) memhog 실행해서 메모리를 왕창 쓰게 해서 swap을 강하게 유도
  int mpid = fork();
  if(mpid < 0){
    printf("forkswap(parent): fork for memhog failed\n");
  } else if(mpid == 0){
    // 자식2: memhog 실행
    char *argv[] = { "memhog", 0 };
    printf("forkswap(memhog child): exec memhog...\n");
    exec("memhog", argv);
    printf("forkswap(memhog child): exec memhog failed\n");
    exit(1);
  }

  // memhog + 앞에서 만든 자식이 동시에 돌면서 swap 많이 발생하도록 기다림
  sleep(400); // 필요하면 값 조정 가능 (ticks 기준)

  int r1 = 0, w1 = 0;
  swapstat(&r1, &w1);
  printf("forkswap(parent): after memhog, swapstat diff: read +%d write +%d\n",
         r1 - r0, w1 - w0);

  // 4) memhog와 자식을 정리
  if(mpid > 0){
    kill(mpid);
  }
  if(cpid > 0){
    kill(cpid);
  }
  // 자식 둘 다 수거
  while(wait(0) >= 0)
    ;

  // 5) 부모 자신의 페이지 내용이 여전히 "부모 패턴"인지 확인
  printf("forkswap(parent): verify my pages (should still have parent pattern)\n");
  for(i = 0; i < NPAGE; i++){
    char *p = pages[i];
    for(j = 0; j < PSIZE; j++){
      uchar expect = parent_pattern(i, j);
      if((uchar)p[j] != expect){
        printf("forkswap: CORRUPTED at page %d, offset %d (got %d, expect %d)\n",
               i, j, (uchar)p[j], expect);
        exit(1);
      }
    }
    if(i % 100 == 0)
      printf("forkswap(parent): verified %d pages\n", i);
  }

  printf("forkswap: ALL OK, parent pages not affected by child\n");
  exit(0);
}

