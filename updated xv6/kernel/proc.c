#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "random.h"
struct mlfq_queue mlfq[5];
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

int max(int a, int b)
{
  return (a > b ? a : b);
}

int min(int a, int b)
{
  return (a < b ? a : b);
}

void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }

  for (int i = 0; i < 5; i++)
  {
    mlfq[i].size = 0;
    mlfq[i].head = 0;
    mlfq[i].tail = 0;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->tickets = 1;
  p->stime = ticks;
  p->rtime = 0;
  p->wtime = 0;
  p->etime = 0;
  p->niceness = 5;
  p->staticp = 60;
  p->numruns = 0;
  p->iotime = 0;
  p->state = USED;
  p->priority = 0;
  p->in_queue = 0;
  p->quanta = 1;
  p->qstime = ticks;
  for (int i = 0; i < 5; i++)
    p->qrtime[i] = 0;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  if ((p->duptrap = (struct trapframe *)kalloc()) == 0)
  {
    release(&p->lock);
    return 0;
  }
  else
  {
    p->ticks = 0;
    p->sighandler = 0;
    p->alarmcheck = 0;
    p->tickstillnow = 0;
    // Set up new context to start executing at forkret,
    // which returns to user space.
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;
  }
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  if (p->duptrap)
    kfree((void *)p->duptrap); // same of the duplicate of the trapframe
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  // p->is_sigalarm = 0; // do we need to do this? No, cuz it is already done is sys_sigalarm
  // p->ticks = 0; // do we need to do this? No, cuz it is already done is sys_sigalarm
  // p->now_ticks = 0; // do we need to do this? No, cuz it is already done is sys_sigalarm
  // p->handler = 0; // do we need to do this? No, cuz it is already done is sys_sigalarm
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->name[0] = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);
  np->tracemask = p->tracemask;
  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  p->etime = ticks;

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

int setpriority(int new, int pid)
{
  struct proc *p;
  int staticcp = -1;
  if (new >= 0 && new <= 100)
  {
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->pid == pid)
      {
        p->niceness = 5;
        staticcp = p->staticp;
        p->staticp = new;
      }
      release(&p->lock);
    }
  }
  return staticcp; // cuz they wont let us have unused variables!!!!!
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
#ifdef RR
  struct proc *p;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        p->numruns++; // Procdump
        p->wtime = 0;

        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }

#else
#ifdef FCFS
  struct proc *p;
  for (;;)
  {
    intr_on();
    int count = 0;
    struct proc *fcfsproc = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        if (count == 0) // for the first allocation
        {
          fcfsproc = p;
          count++;
          continue;
        }
        else if (p->stime < fcfsproc->stime)
        {
          release(&fcfsproc->lock);
          fcfsproc = p;
          count++;
          continue;
        }
      }
      release(&p->lock);
    }
    if (count == 0) // no processes, keep executing
      continue;
    fcfsproc->wtime = 0; // start, initialisng: No waiting till now
    fcfsproc->numruns++;
    fcfsproc->state = RUNNING;              // start the process
    c->proc = fcfsproc;                     // allocate it in the cpu
    swtch(&c->context, &fcfsproc->context); // non-preemptive
    c->proc = 0;                            // over
    release(&fcfsproc->lock);
  }

#else
#ifdef PBS
  struct proc *p;
  for (;;)
  {
    intr_on();
    struct proc *pbsproc = 0;
    int count = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      p->dynamicp = max(0, min(p->staticp - p->niceness + 5, 100));                        // setting the dp
      p->niceness = ((p->endSS - p->startSS) / ((p->endSS - p->startSS) + p->rtime)) * 10; // setting the niceness
      if (p->state == RUNNABLE)
      {
        if (count == 0) // for the first allocation
        {
          pbsproc = p;
          count++;
          continue;
        }
        else if ((p->dynamicp < pbsproc->dynamicp) || (p->dynamicp < pbsproc->dynamicp && p->numruns < pbsproc->numruns) || (p->dynamicp < pbsproc->dynamicp && p->numruns < pbsproc->numruns && p->stime < pbsproc->stime))
        {
          release(&pbsproc->lock);
          pbsproc = p;
          count++;
          continue;
        }
      }
      release(&p->lock);
    }
    if (count == 0)
      continue;

    pbsproc->numruns++;
    pbsproc->wtime = 0;                    // setting the wait time, just like before
    pbsproc->state = RUNNING;              // changing the process state
    c->proc = pbsproc;                     // assigning it to the cpu
    swtch(&c->context, &pbsproc->context); // non-preemptive

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&pbsproc->lock);
  }

#else
#ifdef LBS

  struct proc *p;
  for (;;)
  {
    intr_on();
    int passed_tickets = 0;
    int tot_tickets = 0;

    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
        tot_tickets = tot_tickets + p->tickets;
      // printf("runnable\n");
    }
    long winner = random_at_most(tot_tickets);
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        passed_tickets = passed_tickets + p->tickets;
        if (passed_tickets >= winner)
        {
          c->proc = p;
          p->state = RUNNING;
          swtch(&c->context, &p->context);
          c->proc = 0;
          release(&p->lock);
          break;
        }
        else
        {
          release(&p->lock);
          continue;
        }
      }
      else
      {
        release(&p->lock);
        continue;
      }
    }
  }
#else
#ifdef MLFQ
  struct proc *p;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *mlfqproc = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {
        if (ticks - p->qstime >= 64)
        {
          p->qstime = ticks;
          if (p->in_queue)
          {
            qrm(&mlfq[p->priority], p->pid);
            p->in_queue = 0;
          }
          // p->priority -= (p->priority != 0 ? 1 : 0);
          if (p->priority != 0)
            p->priority--;
        }
      }
    }
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        if (!(p->in_queue))
        {
          push(&mlfq[p->priority], p);
          p->in_queue = 1;
        }
      }
      release(&p->lock);
    }
    for (int levelnum = 0; levelnum < 5; levelnum++) // start from lowest level
    {
      while (mlfq[levelnum].size != 0)
      {
        p = top(&mlfq[levelnum]);
        acquire(&p->lock);
        pop(&mlfq[levelnum]);
        p->in_queue = 0;
        if (p->state == RUNNABLE)
        {
          p->qstime = ticks;
          mlfqproc = p;
          break;
        }
        release(&p->lock);
      }
      if (mlfqproc)
        break; // as we got a process of lower priority value
    }
    if (!mlfqproc)
      continue;
    mlfqproc->state = RUNNING;
    mlfqproc->numruns++;
    c->proc = mlfqproc;
    mlfqproc->quanta = 1 << mlfqproc->priority;
    swtch(&c->context, &mlfqproc->context);
    c->proc = 0;
    mlfqproc->qstime = ticks;
    release(&mlfqproc->lock);
  }
#endif
#endif
#endif
#endif
#endif
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep(), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  p->startSS = ticks;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
        p->endSS = ticks;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
        p->endSS = ticks;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

int myps(void)
{
  printf("PID\t\tPriority\tState\t\t\trtime\t\twtime\t\tnumrun\n");
  struct proc *p;
  char *states[] = {"unused\t", "embryo\t", "sleeping", "runnable", "running\t", "zombie\t"};
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    int waiting_time = ticks - (p->stime + p->rtime + p->iotime);
    printf("%d\t\t%d\t\t%s\t\t%d\t\t%d\t\t%d\n", p->pid, p->dynamicp, states[p->state], p->rtime, waiting_time, p->numruns);
  }
  return 0;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
  myps();
}

uint64 sys_sigalarm(void)
{
  struct proc *p = myproc();
  p->alarmcheck = 0;
  p->tickstillnow = 0;
  p->ticks = p->trapframe->a0;
  p->sighandler = p->trapframe->a1;
  return 0;
}

int waitx(uint64 addr, uint *rtime, uint *wtime)
{
  int pid;
  struct proc *p = myproc();

  acquire(&wait_lock);
  for (;;)
  {
    int havekids = 0;
    for (struct proc *np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->stime - np->rtime;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }
    sleep(p, &wait_lock);
  }
}

void increasertime()
{
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNING)
      p->rtime++;
    else if (p->state == RUNNABLE)
      p->wtime++;
    else if (p->state == SLEEPING)
      p->iotime++;
#ifdef MLFQ
    p->quanta--;
    p->qrtime[p->priority]++;
#endif
    release(&p->lock);
  }
}

struct proc *top(struct mlfq_queue *q)
{
  if (q->head == q->tail)
    return 0;
  return q->procs[q->head];
}

void push(struct mlfq_queue *q, struct proc *element)
{
  if (q->size == NPROC)
    panic("Proccess limit exceeded"); // error handling
  q->procs[q->tail++] = element;
  q->size++;
  if (q->tail == NPROC + 1)
    q->tail = 0;
}

void pop(struct mlfq_queue *q)
{
  if (q->size == 0)
    panic("Empty queue");
  q->head++;
  if (q->head == NPROC + 1)
    q->head = 0;
  q->size--;
}

void qrm(struct mlfq_queue *q, int pid)
{
  for (int curr = q->head; curr != q->tail; curr = (curr + 1) % (NPROC + 1)) // as circular
  {
    if (q->procs[curr]->pid == pid)
    {
      struct proc *temp = q->procs[curr];
      int rqdindex = (curr + 1) % (NPROC + 1);
      q->procs[curr] = q->procs[rqdindex];
      q->procs[rqdindex] = temp;
    }
  }
  q->size--;
  if (q->tail <= 0)
    q->tail = NPROC;
  else
    q->tail--;
}
