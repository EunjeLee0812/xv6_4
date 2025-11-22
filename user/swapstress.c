// user/swapstress.c

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// memhog 몇 개 돌릴지
#define N_MEMHOG   2

// 라운드 수 (몇 번 반복해서 ls/파일읽기/스왑상태 찍을지)
#define ROUNDS     20

// 라운드 사이에 쉬는 시간 (tick 단위, xv6에서 대략 10~100ms 수준)
#define SLEEP_TICKS 50

int
main(void)
{
  int i;
  int memhog_pids[N_MEMHOG];

  printf("swapstress: start\n");

  // 1) 시작할 때 swapstat 기준값 저장
  int r0 = 0, w0 = 0;
  swapstat(&r0, &w0);
  printf("swapstress: initial swapstat: read=%d write=%d\n", r0, w0);

  // 2) memhog 여러 개 실행해서 메모리 + swap 강하게 압박
  for(i = 0; i < N_MEMHOG; i++){
    int pid = fork();
    if(pid < 0){
      printf("swapstress: fork for memhog failed\n");
      // 이미 만든 자식이 있으면 정리
      for(int k = 0; k < i; k++){
        kill(memhog_pids[k]);
        wait(0);
      }
      exit(1);
    }
    if(pid == 0){
      // 자식: memhog 실행
      char *argv[] = { "memhog", 0 };
      printf("swapstress(child %d): exec memhog...\n", i);
      exec("memhog", argv);
      printf("swapstress(child %d): exec memhog failed\n", i);
      exit(1);
    } else {
      // 부모: pid 저장
      memhog_pids[i] = pid;
      printf("swapstress: started memhog[%d] pid=%d\n", i, pid);
    }
  }

  // 3) 부모는 여러 라운드 동안:
  //    - ls 실행 (fork+exec)
  //    - README 조금 읽기
  //    - swapstat 변화 찍기
  for(int round = 0; round < ROUNDS; round++){
    printf("\n===== swapstress: round %d =====\n", round);

    // 3-1) ls 실행해서 파일 시스템 + exec + fork가 살아있는지 확인
    int pid = fork();
    if(pid < 0){
      printf("swapstress: fork for ls failed\n");
      break;
    }
    if(pid == 0){
      char *argv[] = { "ls", 0 };
      exec("ls", argv);
      printf("swapstress: exec ls failed\n");
      exit(1);
    }
    wait(0);  // ls 자식 기다리기

    // 3-2) README 파일 열어서 조금 읽기
    int fd = open("README", O_RDONLY);
    if(fd >= 0){
      char buf[128];
      int n = read(fd, buf, sizeof(buf));
      printf("swapstress: read %d bytes from README\n", n);
      close(fd);
    } else {
      printf("swapstress: failed to open README\n");
    }

    // 3-3) swapstat 찍기
    int r1 = 0, w1 = 0;
    swapstat(&r1, &w1);
    printf("swapstress: swapstat diff: read +%d write +%d (total read=%d write=%d)\n",
           r1 - r0, w1 - w0, r1, w1);

    // 조금 쉬어서 memhog들이 계속 돌게 두기
    sleep(SLEEP_TICKS);
  }

  // 4) 테스트 끝 → memhog 자식들 정리
  printf("\nswapstress: stop memhog children\n");
  for(i = 0; i < N_MEMHOG; i++){
    // 혹시 이미 죽었을 수도 있으니 kill 에러는 신경 안 씀
    kill(memhog_pids[i]);
  }
  for(i = 0; i < N_MEMHOG; i++){
    wait(0);   // 좀비 회수
  }

  // 5) 최종 swapstat 확인
  int rf = 0, wf = 0;
  swapstat(&rf, &wf);
  printf("swapstress: final swapstat: read=%d write=%d (diff: read +%d write +%d)\n",
         rf, wf, rf - r0, wf - w0);

  printf("swapstress: done, returning to shell\n");
  exit(0);
}

