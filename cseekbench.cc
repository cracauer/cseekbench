#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <atomic>
#include <vector>

/*
 * TODO:
 * - SIGINFO
 * - use condition variable to start work in threads, so that thread
 *   creation is not counted for time
 */

std::atomic<int> keep_going(1);
std::atomic<long> count(0);
std::atomic<long> count_bench(0);

void on_timer(int sig)
{
  keep_going = 0;
}

void floatsleep(double seconds) {
  struct timespec req;

  req.tv_sec = (time_t)seconds;
  req.tv_nsec = (long)((seconds - req.tv_sec) * 1e9);

  while (nanosleep(&req, &req) == -1) {
    // nanosleep interrupted, continue sleeping for remaining time
    continue;
  }
}

int just_random(size_t size, off_t blocksize) {
  int ret = 0;

  while (keep_going) {
    double rnd = (double)arc4random() / (double)UINT32_MAX;
    size_t tmp = rnd * (double)(size - blocksize); // truncate
    count_bench++;
    if (tmp == 0)
      ret = 1;  // Prevent compiler from optimizing things away
  }
  return ret;
}

int random_seek_mmap(void *base, size_t size, off_t blocksize) {
  int ret = 0;
  std::vector<std::byte> buf(blocksize);

  while (keep_going) {
    int sum = 0;

    double rnd = (double)arc4random() / (double)UINT32_MAX;
    size_t tmp = rnd * (double)(size - blocksize);
    char *pos = (char *)((size_t)base + tmp);

    if ((long)base < 10000) {
      printf("base too low at %p-%p\n", base, (char *)base + size);
      exit(2);
    }

    sum = *pos;
    memcpy(buf.data(), pos, blocksize);
    count++;
    if (sum == 0)
      ret++;
  }
  return ret;
}

struct thread_mmap_args {
  void *base;
  size_t size;
  off_t blocksize;
};

void *thread_mmap(void *arg) {
  long int ret;
  struct thread_mmap_args *a = (struct thread_mmap_args *)arg;

  ret = random_seek_mmap(a->base, a->size, a->blocksize);

  pthread_exit((void *)ret);
  return (void *)ret;
}

/*
 * Handle both interrupted system calls and short reads
 */
ssize_t myread(int fd, void *buf, size_t nbytes, off_t pos) {
  size_t total = 0;

  while (total < nbytes) {
    int ret;

    switch (ret = pread(fd, (char *)buf + total, nbytes - total, pos + total)) {
    case -1:
      if (errno != EINTR) {
	perror("read");
	exit(2);
      }
      break;
    case 0:
      fprintf(stderr, "EOF reached, not supposed to happen\n");
      exit(3);
    default:
      total += ret;
    }
  }
  return nbytes;
}

int random_seek_syscalls(int fd, size_t size, off_t blocksize, char *buf) {
  int sum = 0;

  if (size == 0) {
    fflush(stdout);
    fprintf(stderr, "Bench size is zero in rand_seek_syscalls\n");
    exit(2);
  }
  //printf("Seek/read size %lu\n", size);
  while (keep_going) {
    double rnd = (double)arc4random() / (double)UINT32_MAX;
    size_t tmp = rnd * ((double)size - (double) blocksize);
    size_t pos = tmp / blocksize;
    pos = pos * blocksize;
    off_t n;

    if ((n = myread(fd, buf, blocksize, pos)) < 0) {
      perror("read");
      exit(2);
    }
    if (n == 0) {
      fprintf(stderr, "EOF reached, not supposed to happen\n");
      exit(3);
    }
    if (n < blocksize) {
      fprintf(stderr, "Short read: %lld/%lld at %ld %f\n"
	      , (long long)n, (long long)blocksize, pos, rnd);
      exit(2);
    }
    sum = buf[0] + buf[blocksize - 1];
    count++;
  }
  return sum;
}

struct thread_syscalls_args {
  int fd;
  size_t size;
  off_t blocksize;
};

void *thread_syscalls(void *arg) {
  struct thread_syscalls_args *a = (struct thread_syscalls_args *)arg;
  char *buf;

  if ((buf = (char *)malloc(a->blocksize)) == NULL) {
    perror("malloc");
    exit(2);
  }
  random_seek_syscalls(a->fd, a->size, a->blocksize, buf);
  free(buf);

  return NULL;
}

struct thread_justrandom_args {
  size_t size;
  off_t blocksize;
};

void *thread_just_random(void *arg) {
  struct thread_justrandom_args *a = (struct thread_justrandom_args *)arg;

  just_random(a->size, a->blocksize);

  return NULL;
}

// with error handling
void my_pthread_create(pthread_t *thread, const pthread_attr_t *attr
		       , void *(*start_routine)(void *), void *arg)
{
  int ret;

  ret = pthread_create(thread, attr, start_routine, arg);
  switch (ret) {
  case 0: return;
  case EAGAIN: fprintf(stderr, "pthread_create: EAGAIN\n"); break;
  case EINVAL: fprintf(stderr, "pthread_create: EINVAL\n"); break;
  case EPERM: fprintf(stderr, "pthread_create: EPERM\n"); break;
  default:  fprintf(stderr, "pthread_create: error %d\n", ret); break;
  }
  exit(2);
}

// with error handling
void my_pthread_join(pthread_t thread, void **retval)
{
  int ret;

  ret = pthread_join(thread, retval);
  switch (ret) {
  case 0: return;
  case EAGAIN: fprintf(stderr, "pthread_join: EAGAIN\n"); break;
  case EINVAL: fprintf(stderr, "pthread_join: EINVAL\n"); break;
  case EPERM: fprintf(stderr, "pthread_join: EPERM\n"); break;
  case EDEADLK: fprintf(stderr, "pthread_join: EDEADLK\n"); break;
  case ESRCH: fprintf(stderr, "pthread_join: ESRCH\n"); break;
  default:  fprintf(stderr, "pthread_join: error %d\n", ret); break;
  }
  exit(2);
}

void usage(FILE *out)
{
  fprintf(out, "usage: different\n");
  exit(1);
}

int main(int argc, char *argv[]) {
  const size_t blocksize = 4096;
  off_t filesize = 0;
  size_t benchsize = 0;
  void *map = NULL;
  double benchtime = 3.0;
  pthread_t *threads = NULL;
  int ch;
  int fd = -1;
  int use_mmap = 0;
  int n_threads = 1;
  int anon = 0;
  int lock = 0;
  int do_bench = 0;

  while ((ch = getopt(argc, argv, "BhlmMs:t:T:")) != -1) {
    switch (ch) {
    // case 'b': blocksize = atol(optarg); break;
    case 'B': do_bench = 1; break;
    case 'h': usage(stdout); break;
    case 'l': lock = 1; break;
    case 'm': use_mmap = 1; break;
    case 'M': anon = 1; break;
    case 's': benchsize = atol(optarg); break;
    case 't': benchtime = atof(optarg); break;
    case 'T': n_threads = atoi(optarg); break;
    default: usage(stderr); break;
    }
  }
  argc -= optind;
  argv += optind;

  switch (argc) {
  case 0: fd = -1; break;
  case 1:
      if ((fd = open(argv[0], O_RDONLY)) < 0) {
	perror("open");
	exit(2);
      }
    break;
  default: usage(stderr);
  }

  threads = (pthread_t *)malloc(sizeof(*threads) * n_threads);
  if (threads == NULL) {
    perror("malloc");
    exit(2);
  }
		   
  if (anon)
    use_mmap = 1;
  if (anon && fd != -1) {
    fprintf(stderr
	    , "When using anon (memory test) no filename should be given\n");
    exit(1);
  }
  if (anon && benchsize == 0) {
    fprintf(stderr
	    , "When using anon (memory test) size must be given\n");
    exit(1);
  }
  if (!anon) {
    if (fd == -1) {
      fprintf(stderr, "When not using anon (memory test) a filename or device "
	      "name must be given\n");
      exit(1);
    }
    if ((filesize = lseek(fd, 0, SEEK_END)) < 0) {
      perror("lseek SEEK_END to determine filesize");
      exit(2);
    }
    if (filesize == 0) {
      fprintf(stderr, "File size is zero, can't work\n");
      exit(2);
    }
    if (benchsize == 0) {
      benchsize = filesize;
    } else {
      if ((off_t)benchsize > filesize) {
	fprintf(stderr, "benchsize larger than filesize\n");
	exit(1);
      }
    }
  }
  printf("File size is %lld, benchsize %lld %.3f GB fd %d\n"
	 , (long long)filesize
	 , (long long)benchsize
	 , (double)benchsize / 1024.0 / 1024.0 / 1024.0
	 , fd);
  if (benchsize < blocksize) {
    fprintf(stderr, "Bench size is smaller than blocksize, can't work\n");
    exit(2);
  }

  struct timeval t;
  void *ret;
  struct thread_justrandom_args args_random;
  struct thread_mmap_args args_mmap;
  struct thread_syscalls_args args_syscalls;
  struct rusage rusage_start;
  struct rusage rusage_end;

  double elapsed_time_bench = 0.0; // gcc falsely warns when uninit
  if (do_bench) {

    keep_going = 1;
    args_random.size = benchsize;
    args_random.blocksize = blocksize;
    if (gettimeofday(&t, NULL) == -1) {
      perror("gettimeofday() failed");
      exit(2);
    }
    for (int i = 0; i < n_threads; i++)
      my_pthread_create(&threads[i], NULL, thread_just_random, &args_random);
  
    double starttime_bench = (double)t.tv_sec + (double)t.tv_usec / 1000000.0;
    floatsleep(benchtime);
    keep_going = 0;
    for (int i = 0; i < n_threads; i++)
      my_pthread_join(threads[i], &ret);
    if (gettimeofday(&t, NULL) == -1) {
      perror("gettimeofday() failed");
      exit(2);
    }
    double endtime_bench = (double)t.tv_sec + (double)t.tv_usec / 1000000.0;
    elapsed_time_bench = endtime_bench - starttime_bench;

    if (count_bench.load() > 0)
      printf("Benchmarked rand(3): %.6f usec/call\n", elapsed_time_bench
	     / (double)count_bench.load() * 1000000);
  }

  keep_going = 1;
  if (use_mmap) {
    if (fd == -1)
	map = mmap(NULL, benchsize, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE
		   , fd, 0);
    else
	map = mmap(NULL, benchsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
      perror("mmap");
      exit(2);
    }
    if (lock == 1) {
      printf("Doing mlock ");
      fflush(stdout);
      if (mlock(map, benchsize) == -1) {
	perror("mlock");
	exit(2);
      }
      printf(".\n");
      fflush(stdout);
    }
    printf("Memory map at %p-%p", map, (char *)map + benchsize);
    if (lock)
      printf(" (locked)");
    printf("\n");

    args_mmap.base = map;
    args_mmap.size = benchsize;
    args_mmap.blocksize = blocksize;
    if (getrusage(RUSAGE_SELF, &rusage_start) == -1) {
      perror("getrusage");
      exit(2);
    }
    if (gettimeofday(&t, NULL) == -1) {
      perror("gettimeofday() failed");
      exit(2);
    }
    for (int i = 0; i < n_threads; i++)
      my_pthread_create(&threads[i], NULL, thread_mmap, &args_mmap);
  } else {
    args_syscalls.fd = fd;
    args_syscalls.size = benchsize;
    args_syscalls.blocksize = blocksize;
    if (getrusage(RUSAGE_SELF, &rusage_start) == -1) {
      perror("getrusage");
      exit(2);
    }
    if (gettimeofday(&t, NULL) == -1) {
      perror("gettimeofday() failed");
      exit(2);
    }
    for (int i = 0; i < n_threads; i++)
      my_pthread_create(&threads[i], NULL, thread_syscalls, &args_syscalls);
  }
  double starttime = (double)t.tv_sec + (double)t.tv_usec / 1000000.0;
  floatsleep(benchtime);
  keep_going = 0;
  for (int i = 0; i < n_threads; i++)
    my_pthread_join(threads[i], &ret);
  if (gettimeofday(&t, NULL) == -1) {
    perror("gettimeofday() failed");
    exit(2);
  }
  if (getrusage(RUSAGE_SELF, &rusage_end) == -1) {
    perror("getrusage");
    exit(2);
  }
  double endtime = (double)t.tv_sec + (double)t.tv_usec / 1000000.0;
  double elapsed_time = endtime - starttime;

  printf("Last ret: %ld, real time %.3f\n", (long int) ret, elapsed_time);
  printf("Count: %ld, %.3f /sec\n", count.load()
	 , (double)count.load() / elapsed_time);
  if (count > 0)
    printf("%.6f usec/access"
	   , (double)elapsed_time / (double)count * 1000000.0);
  if (do_bench)
    printf(" (including arc4random())\n");
  else
    printf("\n");
  if (do_bench)
    if (count.load() > 0 && count_bench.load() > 0)
      printf("%.6f usec/access (subtracted rand() time)\n"
	     , (double)elapsed_time / (double)count.load() * 1000000.0
	     - elapsed_time_bench / (double)count_bench.load() * 1000000
	     );
  printf("Read %.3f %% worth of benchsize\n"
	 , (double)(count.load() * blocksize) / (double)benchsize * 100.0);
  printf("You had %ld minor and %ld major page faults, %ld block inputs\n"
	 , rusage_end.ru_minflt - rusage_start.ru_minflt
	 , rusage_end.ru_majflt - rusage_start.ru_majflt
	 , rusage_end.ru_inblock - rusage_start.ru_inblock
	 );

  if (fd != -1)
    if (close(fd) == -1) {
      perror("close");
      exit(2);
    }
  if (use_mmap)
    if (munmap(map, benchsize) == -1) {
      perror("munmap");
      exit(2);
    }
  free(threads);
  threads = NULL;

  return 0;
}
