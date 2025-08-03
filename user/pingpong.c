#include "kernel/types.h"
#include "user/user.h"

int main(){
    int p1[2]; // pipe for parent to child
    int p2[2]; // pipe for child to parent
    char buf[1];

    pipe(p1);
    pipe(p2); 

    int pid = fork();
    if(pid < 0){
        fprintf(2, "fork failed\n");
        exit(1);
    }
    else if(pid==0){
        // child process
        read(p1[0], buf, 1); // read from parent
        printf("%d: received ping\n", getpid());
        write(p2[1], buf, 1); // send pong to parent
        close(p1[0]);
        close(p2[1]);
        exit(0);
    }
    else{
        // parent process
        write(p1[1],"a", 1); // send ping to child
        read(p2[0], buf, 1); // wait for pong from child
        printf("%d: received pong\n", getpid());
        close(p1[1]);
        close(p2[0]);
        wait(0); // wait for child to finish
        exit(0);
    }
}