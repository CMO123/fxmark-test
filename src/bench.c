#include <sys/time.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bench.h"
#include "cpupol.h"
#include "rdtsc.h"

static struct bench *running_bench;

static uint64_t usec(void)
{
        struct timeval tv;
        gettimeofday(&tv, 0);
        return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static inline void nop_pause(void)
{
        __asm __volatile("pause");
}

static inline void wmb(void)//内存屏障
{// memory强制gcc编译器假设RAM所有内存单元均被汇编指令修改，这样cpu中的registers和cache中已缓存的内存单元中的数据将作废。
// cpu将不得不在需要的时候重新读取内存中的数据。这就阻止了cpu又将registers，cache中的数据用于去优化指令，而避免去访问内存。 
        __asm__ __volatile__("sfence":::"memory");
}

static int setaffinity(int c)
{
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(c, &cpuset);
        return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

struct bench *alloc_bench(int ncpu, int nbg)
{//分配bench和workers结构
//该函数调用mmap()分配一段内存（使用共享内存是因为测试时，每个进程都需要访问这段内存），
//fd为-1表示使用匿名内存映射，即fd可忽略。大小是一个bench结构的大小加上ncpu个worker结构的大小
//bench结构记录了配置的信息，如cpu核数、测试持续时间、I/O模式。由主进程管理
//worker结构记录了测试的实际时间、操作完成次数，返回值等信息。测试过程中每个子进程会管理一个worker
        struct bench *bench; 
        struct worker *worker;
        void *shmem;
        int shmem_size = sizeof(*bench) + sizeof(*worker) * ncpu;
        int i;
        
        /* alloc shared memory using mmap */
        shmem = mmap(0, shmem_size, PROT_READ | PROT_WRITE, 
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shmem == MAP_FAILED)
                return NULL;
        memset(shmem, 0, shmem_size);

        /* init. */ 
        bench = (struct bench *)shmem;
        bench->ncpu = ncpu; 
        bench->nbg  = nbg;
        bench->workers = (struct worker*)(shmem + sizeof(*bench));
        for (i = 0; i < ncpu; ++i) {
                worker = &bench->workers[i];
                worker->bench = bench;
                worker->id = seq_cores[i];
		worker->is_bg = i >= (ncpu - nbg);
        }

        return bench;
}

static void sighandler(int x)
{
        running_bench->stop = 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"

int if_rdtscp(){//判断CPU是否支持rdtscp，1支持，0不支持
	unsigned int edx;
		unsigned int eax = 0x80000001;
#ifdef __GNUC__              // GNU extended asm supported
		__asm__ (	  // doesn't need to be volatile: same EAX input -> same outputs
		 "CPUID\n\t"
		: "+a" (eax),		  // CPUID writes EAX, but we can't declare a clobber on an input-only operand.
		  "=d" (edx)
		: // no read-only inputs
		: "ecx", "ebx");	  // CPUID writes E[ABCD]X, declare clobbers
	
		// a clobber on ECX covers the whole RCX, so this code is safe in 64-bit mode but is portable to either.
	
#else // Non-gcc/g++ compilers.
		// To-do when needed
#endif
		return (edx >> 27) & 0x1;


}

static void worker_main(void *arg)
{
        struct worker *worker = (struct worker*)arg;
        struct bench *bench = worker->bench;
        uint64_t s_clk = 1, s_us = 1;
        uint64_t e_clk = 0, e_us = 0;
        int err = 0;

        /* set affinity */ 
        setaffinity(worker->id);


        /* pre-work *///准备测试文件
        if (bench->ops.pre_work) {
                err = bench->ops.pre_work(worker);
                if (err) 
					{
					fprintf(stdout,"pre_work error\n");
					goto err_out;
                	}
        }

        /* wait for start signal */ 
        worker->ready = 1;//子进程准备文件完成后，设置自己的worker->ready = 1;
        if (worker->id) {//检查子进程的start成员，看是否能开始，不能则等待
                while (!bench->start)
                        nop_pause();
        }
	else {
		/* are all workers ready? *///主进程准备文件完成后，会检查所有子进程worker的实例ready是否都为1，当所有子进程都准备好之后，会将bench的start设置为1，
		//表示可以开始测试了，通过上述方式，所有进程可以在完成准备工作之后一起开始测试，实现同步。
		int i;
		for (i = 1; i < bench->ncpu; i++) {
			struct worker *w = &bench->workers[i];
			while (!w->ready)
				nop_pause();
		}
		/* make things more deterministic */
		sync();

		/* start performance profiling */
		if (bench->profile_start_cmd[0])
			system(bench->profile_start_cmd);

                /* ok, before running, set timer */
                if (signal(SIGALRM, sighandler) == SIG_ERR) {
                        err = errno;
						//fprintf(stdout,"signal(SIGALRM, sighandler) error \n");
                        goto err_out;
                }
                running_bench = bench;
                alarm(bench->duration);
                bench->start = 1;
                wmb();
        }
       
        /* start time */
		//fprintf(stdout,"if_rdtscp = %d\n", if_rdtscp());
        s_clk = rdtsc_beg();
        s_us = usec();



        /* main work *///在main_work进行测试的前后，每个进程会记录时间，如果bench->ops.post_work()不为null，每个进程还会调用函数进程一些收尾工作。
        if (bench->ops.main_work) {
                err = bench->ops.main_work(worker);
                if (err && err != ENOSPC)
                	{
                		//fprintf(stdout, "main_work() error\n");
                        goto err_out;
                	}
        }


        /* end time */ 
        e_clk = rdtsc_end();

        e_us = usec();

	/* stop performance profiling */
        if (!worker->id && bench->profile_stop_cmd[0])
		system(bench->profile_stop_cmd);


        /* post-work */ 
        if (bench->ops.post_work){
                err = bench->ops.post_work(worker);
        	}
err_out:
        worker->ret = err;
        worker->usecs = e_us - s_us;
        wmb();
        worker->clocks = e_clk - s_clk;
}

static void wait(struct bench *bench)
{
        int i;
        for (i = 0; i < bench->ncpu; i++) {
                struct worker *w = &bench->workers[i];
                while (!w->clocks)
                        nop_pause();
        }
}

void run_bench(struct bench *bench)
{//该函数会根据配置信息中的cpu个数派生子进程，加上主进程，所有ncpu个进程调用worker_main()进行测试
        int i;
	for (i = 1; i < bench->ncpu; ++i) {
		/**
		 * fork() is intentionally used instead of pthread
		 * to avoid known scalability bottlenecks 
		 * of linux virtual memory subsystem. 
		 */ 
		//fprintf(stdout, "bench->ncpu = %d, after fork()\n",bench->ncpu);
		pid_t p = fork();
		
		if (p < 0){
			bench->workers[i].ret = errno;
			//fprintf(stdout, "pid p = fork() < 0\n");
		}
		else if (!p) {
			worker_main(&bench->workers[i]);
			exit(0);
		}
	}
	worker_main(&bench->workers[0]);
	wait(bench);
}

void report_bench(struct bench *bench, FILE *out)
{
	static char *empty_str = "";
        uint64_t total_usecs = 0;
        double   total_works = 0.0;
        double   avg_secs;
	char *profile_name, *profile_data;
        int i, n_fg_cpu;

        /* if report_bench is overloaded */ 
        if (bench->ops.report_bench) {
                bench->ops.report_bench(bench, out);
                return;
        }

        /* default report_bench impl. */
        for (i = 0; i < bench->ncpu; ++i) {
                struct worker *w = &bench->workers[i];
		if (w->is_bg) continue;
                total_usecs += w->usecs;
                total_works += w->works;
        }
	n_fg_cpu = bench->ncpu - bench->nbg;
        avg_secs = (double)total_usecs/(double)n_fg_cpu/1000000.0;

	/* get profiling result */ 
	profile_name = profile_data = empty_str;
	if (bench->profile_stat_file[0]) {
		FILE *fp = fopen(bench->profile_stat_file, "r");
		size_t len;
		
		if (fp) {
			profile_name = profile_data = NULL;
			getline(&profile_name, &len, fp);
			getline(&profile_data, &len, fp);
			fclose(fp);
		}
	}

        fprintf(out, "# ncpu secs works works/sec %s\n", profile_name);
        fprintf(out, "%d %f %f %f %s\n", 
                n_fg_cpu, avg_secs, total_works, total_works/avg_secs, profile_data);

	if (profile_name != empty_str)
		free(profile_name);
	if (profile_data != empty_str)
		free(profile_data);
}

#pragma GCC diagnostic pop
