#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"
#include "kernel/types.h"
#include "kernel/param.h"

#define PGSIZE 4096

static int is_printable(char c) {
  return c >= 32 && c <= 126;
}

// secret 是 7 个可打印字符 + '\0' 组成的 8 字节
static int looks_like_secret(const char *p) {
  for (int i = 0; i < 7; i++) {
    if (!is_printable(p[i])) return 0;
  }
  return p[7] == '\0';
}

int
main(int argc, char *argv[])
{
  // your code here.  you should write the secret to fd 2 using write
  // (e.g., write(2, secret, 8)
 const int max_tries = 4096;  // 可按需调大/调小
  for (int t = 0; t < max_tries; t++) {
    char *page = (char*)sbrk(PGSIZE);
    if ((long)page == -1) {
      // 分配失败就结束
      break;
    }
    char *p = page + 32;  // secret.c 将 8 字节写在页首 + 32
    if (looks_like_secret(p)) {
      // 严格写 8 字节到 fd=2（包含 '\0'），不要多写换行
      write(2, p, 8);
      exit(0);
    }
  }

  exit(1);
}

