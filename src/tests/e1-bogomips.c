/*
 * dumbflation.c - measure BogoMIPS the 1993 way on a modern ARM core.
 *
 * Linux 0.99.11 calibrated a busy loop at boot and printed the result as
 * BogoMIPS: "the number of million times per second a processor can do
 * absolutely nothing." Modern ARM kernels don't do this any more. They derive
 * the value from the architected timer frequency, so the number in
 * /proc/cpuinfo describes a clock that has nothing to do with the CPU.
 *
 * This runs the original delay loop and applies the original formula, so the
 * result is comparable with the values people reported in the BogoMips
 * mini-HOWTO in the 1990s.
 *
 *   BogoMIPS = loops_per_second / 500000
 *
 * The divisor is 500000 rather than a million because the classic loop body
 * was two instructions: decrement, branch.
 *
 * Build for the camera:
 *   arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a7 -o dumbflation dumbflation.c
 *
 * Build natively on anything else:
 *   cc -O2 -o dumbflation dumbflation.c
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * The delay loop. The kernel's version was written in assembly so the compiler
 * could not optimise it away or unroll it, which would change what is being
 * measured. Same reasoning here.
 */
static void bogo_delay(unsigned long loops)
{
#if defined(__arm__) || defined(__aarch64__)
	__asm__ __volatile__(
		"1: subs %0, %0, #1\n"
		"   bhi 1b\n"
		: "+r"(loops)
		:
		: "cc");
#elif defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__(
		"1: dec %0\n"
		"   jns 1b\n"
		: "+r"(loops)
		:
		: "cc");
#else
	volatile unsigned long n = loops;
	while (n--) {
		/* nothing, as intended */
	}
#endif
}

static double seconds_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct worker {
	unsigned long loops;
	double        elapsed;
};

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	double start = seconds_now();
	bogo_delay(w->loops);
	w->elapsed = seconds_now() - start;
	return NULL;
}

/*
 * Doing nothing is the one workload that parallelises perfectly, so measure it
 * on every core at once rather than multiplying the single-core figure. The
 * kernel used to print exactly this as "Total of N processors activated".
 */
static double measure_all_cores(unsigned long loops, int ncpu)
{
	pthread_t     *tids = calloc((size_t)ncpu, sizeof(*tids));
	struct worker *ws   = calloc((size_t)ncpu, sizeof(*ws));
	double slowest = 0.0;

	for (int i = 0; i < ncpu; ++i) {
		ws[i].loops = loops;
		pthread_create(&tids[i], NULL, worker_main, &ws[i]);
	}
	for (int i = 0; i < ncpu; ++i) {
		pthread_join(tids[i], NULL);
		if (ws[i].elapsed > slowest) {
			slowest = ws[i].elapsed;
		}
	}

	double total = ((double)loops * (double)ncpu) / slowest / 500000.0;
	free(tids);
	free(ws);
	return total;
}

int main(void)
{
	unsigned long loops = 1UL << 12;
	double elapsed = 0.0;

	/* Grow the loop count until one run takes long enough to time honestly. */
	while (elapsed < 0.25) {
		double start = seconds_now();
		bogo_delay(loops);
		elapsed = seconds_now() - start;

		if (elapsed < 0.25) {
			loops <<= 1;
		}
	}

	/* Best of five, to shake off scheduling noise. */
	double best = elapsed;
	for (int i = 0; i < 4; ++i) {
		double start = seconds_now();
		bogo_delay(loops);
		double run = seconds_now() - start;
		if (run < best) {
			best = run;
		}
	}

	double loops_per_second = (double)loops / best;
	double bogomips = loops_per_second / 500000.0;

	printf("loops            : %lu\n", loops);
	printf("best run         : %.4f s\n", best);
	printf("loops per second : %.0f\n", loops_per_second);
	printf("BogoMIPS         : %.2f\n", bogomips);
	printf("\n");
	int ncpu = 1;
#ifdef _SC_NPROCESSORS_ONLN
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online > 1) {
		ncpu = (int)online;
	}
#endif
	double total = measure_all_cores(loops, ncpu);

	printf("cores online     : %d\n", ncpu);
	printf("total BogoMIPS   : %.2f\n", total);
	printf("\n");
	printf("A 386DX/33 reported 6.65 in the BogoMips mini-HOWTO.\n");
	printf("Per core, this machine is %.0fx better at doing nothing.\n", bogomips / 6.65);
	printf("All cores at once, %.0fx.\n", total / 6.65);

	return 0;
}
