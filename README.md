# cseekbench
Filesystem and raw device seek benchmark


cseekbench is a very simple I/O benchmark: it seeks to a random
position and reads 4k.  It does it in a loop and reports the average
time.

You can use it on:
- regular files
- block devices including disks and NVMe (most OSes)
- anonymous memory

It does not modify anything.  It should be perfectly safe to run it on
a block device that has a filesystem on it, even a mounted one.  I do
that all the time.

# Usage

-B before the actual I/O benchmark, benchmark the random function and
later subtract it from the I/O time.  This works badly in
multithreaded mode.  It is not necessary for actual I/O devices, but
it will help if you use this benchmark for RAM or CPU cache
measurements.

-h help

-l do an mlock(2) on the mmap'ed region.  Has no effect when using
seek/read.

-m use mmap(2) instead of seek/read.  The default is seek/read.  The
difference is usually significant.

-M use anonymous memory (implies -m) instead of a file or device.

-R <init> initialize the random number generator to <init>.  This
leads to reproducible seek locations.

-s <n> size of the benchmark in bytes.  Default is file or device
size.  Must be given when using -M.

-t <secs> benchmark time in seconds (floating point supported).
Default is 3 seconds.

-T <n> use <n> threads.  Default is 1.  Works badly for memory
benchmarking. 

# Examples:

$ cseekbench myfile

Benchmark inside myfile with size equal to the file size.  The file
should be big enough that only a fraction of it is read during the
whole benchmark, otherwise you get most of it out of the VM page
cache.

$ cseekbench /dev/ada0

Benchmark inside block device ada0.  Various combinations of mmap or
seek/read work with various OSes.  Linux supports both, FreeBSD
supports only seek/read, macOS supports neither.

$ cseekbench -B -t 0.01 -M -l -s $((8 * 1024 * 1024 * 1024))

Do a memory benchmark by allocating 8 GB of anonymous mmap(2)ed
memory, mlock(2) it into RAM and run for a hundredth of a second.
Benchmark the random function first and subtract that time from the
result.

# Notes

Be careful with the -l (mlock) option.  It can violently push other
things out of RAM.  You might have to raise ulimits to lock as much
memory as you wish.  If you use the systemd OOM killer you will
probably use valuable process groups instead of just the benchmark.
You should switch back to the kernel OOM killer.

Dropping filesystem caches before benchmarking improves results.  If
you just wrote the file and it is smaller than RAM you must do this.
In general it is recommended that the file size is 1.5 times RAM if
you can't drop the caches.

You can find out how much of the file is in RAM with my clockmem
utility.

# Future improvements

I have several ideas to cut down the overhead from the benchmark's
machinery some more so that usefulness as a memory benchmark improves.
It looks pretty good already, though.

Turn this file into a manpage.
