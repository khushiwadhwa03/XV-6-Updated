# MODIFYIED XV-6 OPERATING SYSTEM


## Overview

The changes made to the XV-6 operating system are as follows :

1. `strace` ,`sigalarm` , `sigreturn` and `waitx` system calls added .
2. `Scheduling` techniques such as `RR`,`FCFS`, `PBS`,`MLFQ` and `Lottery based scheduling` have  been implemented.
3. `Copy-on-Write Fork` implemented.

## Running the xv6 OS

- In the command line, run `make qemu SCHEDULER=[OPTIONS]`
- The `[OPTIONS]` are `RR - ROUND ROBIN`(default if nothing is written) , `MLFQ-MULTI LEVEL SCHEDULING QUEUE`, `PBS - PRIORITY BASED SCHEDULING` and `FCFS - FIRST COME FIRST SERVE`


# TASK 1

## strace syscall

- Implemented a system call, strace, with accompanying userprogram strace.
- It intercepts and records the system calls which are called by a process during its execution.
- It takes one argument, an integer mask, whose bits specify which system calls to trace.

For adding the syscall , the following changes were made :
1. In the file `syscall.h` we  defined a new syscall strace
2. In the file `sysproc.c` we added the strace handler, for passing arguments to the system call
3. In the file `syscall.c` we  modified the syscall function to print the details of the system calls specified by the mask
5. In the file  `proc.h`  we added variable `int mask` in the struct proc
6. In the File `proc.c` we modified the `fork()` function and copied the mask from parent to the child process
7. In the file `user.h`  we defined the function `int trace(int ,int )`
8. We made a  file `strace.c` which is a  userinterface invoking the `trace` system call
9. Added `$U/_strace\` in `UPROGS` in the Makefile.
10.  In the file `usys.pl` we added `entry("trace");`



# TASK 2

To implement the required schedulers the CFLAG for the SCHEDULER was added in the makefile , along with the required #ifdef and #endif statements.

## RR SCHEDULING
As RR is the default scheduler , we added the entire scheduler code into the RR section


## FCFS SCHEDULING

- The FCFS policy selects the process with the lowest creation time , and runs it till no more CPU time is needed.
- To run the OS with FCFS scheduling , enter the command `make qemu SCHEDULER=FCFS`

- the changes made to implement this were:
     - used startproc in struct proc .
     - created a structure  that stores the process with the minimum creation time
     - The algorithm for the FCFS scheduler acquires the lock of all  processes that are currently runnable , and compares their startTime  with with the process stored in $$$$ .
     - the lock was released only when a better candiate for $$$$ was found.
     - Once we got the process with the minimum run time , the CPU's context was switched to that process's context , and its state was updated to running , and the lock of the process  is released.
     - in the initial case , where the process is runnable , and there is no process in $$$$ , the process is added to $$$$
     - when no process was found , we continue.

## PBS SCHEDULER
- A non-preemptive priority-based scheduler that selects the process with the highest priority for execution.
- To run the OS with FCFS scheduling , enter the command `make qemu SCHEDULER=PBS`

- The default proriy of processes is 60.
- In case two or more processes have the same priority, we use the number of times the process has been scheduled to break the tie. If the tie remains, we use the start-time of the process to break the tie .
- The changes made to implement it are :
  -
  - All newly created processes are assigned priority 60 and niceness 5 in allocproc, This is the static priority used in proc.c
  - We store the time when the process stars sleeping , stops sleeping and the ticks spent in the running time in the variables $$ $$ and $$ respectively.
  - The function $$$ updates the dynamic priority of the process using its nicess, the time it slept for and its running time.
  - In the function setpriority(), we iterated all the process to find the process with pid equal to the given value & updated its priority . The function returns the old priority of the process.


## LOTTERY BASED SCHEDULER

- a preemptive scheduler that assigns a time slice to the process randomly in proportion of the number of tickets it holds.
- To run the OS with FCFS scheduling , enter the command `make qemu SCHEDULER=LBS`

- to implement it , we made the following changes : 
  - made a  syscall name settickets() , 
  which is responsible for setting the tickets for a process
  - include random.c and random.h - files that generate random numbers.
  - we use the above to generate a random number less than the total number of tickets
  - keep a track of the number of tickets passed while looping through the processes , and start running the process as soon as this count exceeds the random value we got earlier.
  - terminate the loop once we run a process.

## MULTI LEVEL FEEDBACK QUEUE SCHEDULER ( MLFQ )

- a simplified preemptive MLFQ scheduler that allows processes to move between different priority queues based on their behavior and CPU bursts.
- To run the OS with FCFS scheduling , enter the command `make qemu SCHEDULER=MLFQ`

- to implement this scheduler , we made the following changes :
  - created variables inside struct proc which store allocated time , priority , time spent in the current queue , and the time passed since it joined the current queue.
  - Created 5 queues of decreasing priority , and  having increasing timer ticks .
  - edited scheduler() and added the algorithm that shifts the processes in between the queues based on priority , and run time.Implemented aging in the same.
  - added flags in usertrap() and kerneltrap() which are used to yield as soon as a process exhausts its time slice.


# ANALYSIS


| scheduler    | rtime | wtime |
| :-----------:|:-----:|:-----:|
| RR           |  15   |  160  |
| PBS          |  15   |  131  |
| FCFS         |  38   |  170  |
| LBS          |  14   |  140  |
| MLFQ         |  34   |  228  |

on running  schedulertest for each of the schedulers , we get the above results