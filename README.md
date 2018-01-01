# iix

iix - interactive input extender

### Synopsis
```
iix [options] program [arguments]
```

### Options
```
 -f file        read from a file
 -p pipe        read from a named pipe
```

### Description
iix writes data received from standard input, and any specified files or named pipes, to the master end of a pseudo-terminal whose slave end is used for the standard I/O streams of a child process that executes a requested program. Data received from the child process, via the master end of the pseudo-terminal, is written to the parent process's standard output stream.

### Distribution
```
$ autoreconf -i
$ ./configure
$ make distcheck
$ make maintainer-clean
```

### Installation
```
$ tar xvf iix-0.1.tar.gz
$ cd iix-0.1
$ ./configure
$ make
# make install
```

### Example
NB: (1) and (2) denote separate terminals, and bracketed text indicates unprompted input.                     

```
(1) $ mkfifo /tmp/pipe
(1) $ iix -p /tmp/pipe -- gdb -q xxd
    Reading symbols from xxd...(no debugging symbols found)...done.
    (gdb) b exit
    Function "exit" not defined.
    Make breakpoint pending on future shared library load? (y or [n]) y
    Breakpoint 1 (exit) pending.
    (gdb) r
    Starting program: /usr/bin/xxd

(2) $ printf "AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD" > /tmp/pipe

(1) AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD
    [^D]
    00000000: 4141 4141 4141 4141 4242 4242 4242 4242  AAAAAAAABBBBBBBB
    00000010: 4343 4343 4343 4343 4444 4444 4444 4444  CCCCCCCCDDDDDDDD

(2) $ printf "EEEEEEEEFFFFFFFFGGGGGGGGHHHHHHHH" > /tmp/pipe

(1) EEEEEEEEFFFFFFFFGGGGGGGGHHHHHHHH
    [^D]
    00000020: 4545 4545 4545 4545 4646 4646 4646 4646  EEEEEEEEFFFFFFFF
    00000030: 4747 4747 4747 4747 4848 4848 4848 4848  GGGGGGGGHHHHHHHH
    [^D]

    Breakpoint 1, __GI_exit (status=0) at exit.c:105
    105     exit.c: No such file or directory.
    (gdb) quit
    A debugging session is active.

            Inferior 1 [process 16749] will be killed.

    Quit anyway? (y or n) y
```