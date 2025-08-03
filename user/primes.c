#include"kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 递归函数，不会返回
void primes(int *p) __attribute__((noreturn));

void primes(int *p) {
    int n; // 选定本次筛选基数
    if(read(p[0], &n, sizeof(int)) != sizeof(int)) {
        close(p[0]);  // 关闭读端
        exit(0);
    }
    
    fprintf(1, "prime %d\n", n); // 输出筛选基数

    int pright[2];
    pipe(pright); // 与下一个进程相连的管道

    if(fork() == 0) {
        close(p[0]); // 关闭读端
        close(pright[1]); // 子进程只读
        primes(pright); 
    }
    else {
        close(pright[0]); // 父进程只写
        int m;// 筛选剩下的数
        while(read(p[0], &m, sizeof(int)) == sizeof(int)) {
            if(m % n != 0) {
                write(pright[1], &m, sizeof(int)); // 写入下一个管道
            }
        }
        close(p[0]); // 关闭读端
        close(pright[1]); // 关闭写端
        wait(0); // 等待子进程结束
    }

    exit(0);
}

int main(){
    int p[2];
    pipe(p);

    if(fork() == 0){
        close(p[1]); // 子进程只读
        primes(p);
    } else {
        close(p[0]); // 父进程只写

        for(int j = 2; j <= 280; j++) {
            write(p[1], &j, sizeof(j)); // 写入管道
        }

        close(p[1]); // 关闭写端
        wait(0);

    }

    exit(0);
}