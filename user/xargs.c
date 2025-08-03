#include"kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int
read_line(char* buf,int max){
    int i=0;
    char c;
    while(i<max-1){
        int n=read(0,&c,1);
        if(n==0){
            break;
        }//EOF
        if(c=='\n'){
            break;
        }//end of line
        buf[i++]=c;
    }
    buf[i]=0;//string end with 0
    return i>0;
}

void
parse_line(char* line,char** argv,int* agrc){
    char* p=line;
    while(*p!=0){
        //skip space
        while(*p==' '){
            p++;
        }
        if(*p==0){
            break;
        }

        //start of argument
        argv[*agrc]=p;
        (*agrc)++;

        //move to the end of argument
        while(*p!=0&&*p!=' ')p++;
        //p==0 quit
        if(*p==0)break;
        //p==' ' division arguments
        *p=0;
        p++;
    }
}

int
main(int argc,char *argv[]){
    if (argc < 2) {
        fprintf(2, "Usage: xargs <command> [initial-args...]\n");
        exit(1);
    }
    char buf[512];
    char *exec_argv[MAXARG];

    // copy initialized arguments(except "axrgs")
    int i;
    for(i=0;i<argc-1;i++){
        exec_argv[i]=argv[i+1];
    }
    while(read_line(buf,sizeof(buf))){
        int argi=i;//追加参数从i开始
        parse_line(buf, exec_argv, &argi);
        exec_argv[argi] = 0;  // 参数向量 0 结尾

        if(fork()==0){
            exec(exec_argv[0],exec_argv);
            fprintf(2, "xargs: exec failed\n");
            exit(1);
        }
        else{
            wait(0);
        }

    }

    exit(0);
}