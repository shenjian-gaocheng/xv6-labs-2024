#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  // print kernel stack backtrace
  backtrace();

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_sigalarm(void)
{
  int ticks;
  argint(0, &ticks);

  if (ticks < 0){
    return -1;
  }

  // handler might be zero in user space
  uint64 handler;
  argaddr(1, &handler);

  struct proc *p = myproc();
  p->alarm_elapse = 0;
  p->alarm_interval = ticks;
  p->alarm_handler = handler;

  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  p->alarm_elapse = 0;

  // restore caller-saved registers
  // see kernel/trap.c:useralarm()
  p->trapframe->t0 = p->alarm_context.t0;
  p->trapframe->t1 = p->alarm_context.t1;
  p->trapframe->t2 = p->alarm_context.t2;
  p->trapframe->t3 = p->alarm_context.t3;
  p->trapframe->t4 = p->alarm_context.t4;
  p->trapframe->t5 = p->alarm_context.t5;
  p->trapframe->t6 = p->alarm_context.t6;
  p->trapframe->a0 = p->alarm_context.a0;
  p->trapframe->a1 = p->alarm_context.a1;
  p->trapframe->a2 = p->alarm_context.a2;
  p->trapframe->a3 = p->alarm_context.a3;
  p->trapframe->a4 = p->alarm_context.a4;
  p->trapframe->a5 = p->alarm_context.a5;
  p->trapframe->a6 = p->alarm_context.a6;
  p->trapframe->a7 = p->alarm_context.a7;

  return p->trapframe->a0;
}