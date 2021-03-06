/*
 * POSIX Real Time Example
 * using a single pthread as RT thread
 */

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <sys/io.h>
#include <signal.h>
#include <stdbool.h>
#include "global.h"
#include "sm_driver.h"
#include "speed_cntr.h"
#include "options.h"

// Global status flags
struct GLOBAL_FLAGS status = {FALSE, FALSE, 0};

// 2PI
#define ONE_TURN	(2*3.1416*100)

int running = true;
int rt_thread_started = false;
int total_step_count = 0;
int total_steps;

void signalHandler(int sig)
{
	running = false;
}

struct period_info {
        struct timespec next_period;
        long period_ns;
};
 
static void inc_period(struct period_info *pinfo) 
{
        pinfo->next_period.tv_nsec += pinfo->period_ns;
 
        while (pinfo->next_period.tv_nsec >= 1000000000) {
                /* timespec nsec overflow */
                pinfo->next_period.tv_sec++;
                pinfo->next_period.tv_nsec -= 1000000000;
        }
}
 
static void periodic_task_init(struct period_info *pinfo)
{
        /* for simplicity, hardcoding a 1ms period */
        /* pinfo->period_ns = 1000000; */

	/* hardcoding a 2.17us = 2170 ns = 460.75khz */
        //pinfo->period_ns = 2170;
        
	/* 217000ns = 4.6khz */
        pinfo->period_ns = 217000;
 
        clock_gettime(CLOCK_MONOTONIC, &(pinfo->next_period));
}
 
static void wait_rest_of_period(struct period_info *pinfo)
{
        inc_period(pinfo);
 
        /* for simplicity, ignoring possibilities of signal wakes */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pinfo->next_period, NULL);
}


/* stop time must be larger than start time */
void timespec_diff(struct timespec *start, struct timespec *stop,
                   struct timespec *result)
{
    if ((stop->tv_nsec - start->tv_nsec) < 0) {
        result->tv_sec = stop->tv_sec - start->tv_sec - 1;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
    } else {
        result->tv_sec = stop->tv_sec - start->tv_sec;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec;
    }

    return;
}

void print_x(int n)
{
	while (n>0){
		printf("*");
		n--;
	}
	printf("\n");

}

void *simple_cyclic_task(void *data)
{
        struct period_info pinfo;
	int count = 0;
	int rc;
	int current_time = 0;
	
	printf("%s started\n", __FUNCTION__);	 
        periodic_task_init(&pinfo);
        while (running){
		rt_thread_started = true;
		/* Time/counter enabled */
		if ((TCCR1B & (1<<CS11)) && (OCR1A > 0)){
			count++;
			/* timer/counter compare output */
	                if (count >= OCR1A){
				/* reset count */
				count = 0;
				/* do realtime task */
				rc = speed_cntr_TIMER1_COMPA_interrupt();
				switch(rc){
					case NOACT:
						break;
					case CW:
					case CCW:
						current_time++;
						total_step_count++;
						break;
				}
			}
		}
		else{
			/* timer/counter disabled */
			count = 0 ;
		}
                wait_rest_of_period(&pinfo);
		if (total_step_count >= total_steps)
			break;
        }
 
        return NULL;
}
 
int main(int argc, char* argv[])
{
        struct sched_param param;
        pthread_attr_t attr;
        pthread_t thread;
        int ret;
	unsigned int accel, decel, speed;
	int n;
	struct motor_options p = { 
		5.0, /* 5 turn */
		1.0, /* accel = 1 turn/sec*sec */
		1.0, /* decel = 1 turn/sec*sec */
		1.0  /* speed = 1 turn/sec */
	};

	if (!get_motor_options(argc, argv, &p)){
		exit (0);
	}

	printf("--------------------------------------------------\n");
	printf(" Total number of turn : %4.4f \n", p.turn);
	printf("         Acceleration : %4.4f turn/sec*sec\n", p.accel);
	printf("        Decceleration : %4.4f turn/sec*sec\n", p.decel);
	printf("                Speed : %4.4f turn/sec\n", p.speed);
	printf("--------------------------------------------------\n");

	/* initialize, ie stop timer/counter, must be init before speed_cntr_Move */
	speed_cntr_Init_Timer1();

	/* Move motor */
	total_steps = (unsigned int)(p.turn * SPR);
	accel = (unsigned int)(p.accel * ONE_TURN);
	decel = (unsigned int)(p.decel * ONE_TURN);
	speed = (unsigned int)(p.speed * ONE_TURN);
	printf("speed_cntr_Move(%d, %d, %d, %d)\n",
		total_steps, accel, decel, speed);
	speed_cntr_Move(total_steps, accel, decel, speed);

	/* initialize parallel port */
	printf("Parallel Port Interface (Base: 0x%x)\n", BASE);
	
	// Set permission bits of 4 ports starting from BASE
	if (ioperm(BASE, 4, 1) != 0){
		printf("ERROR: Could not set permissions on ports\n");
		return 0;
	}

	/* initialize io port, must be init after parallel is initialized */
	sm_driver_Init_IO();

	/* ctrl-c handler */
	signal(SIGINT, signalHandler);

        /* Lock memory */
        if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
                printf("mlockall failed: %m\n");
		// Clear permission bits of 4 ports starting from BASE
		ioperm(BASE, 4, 0);
                exit(-2);
        }

        /* Initialize pthread attributes (default values) */
        ret = pthread_attr_init(&attr);
        if (ret) {
                printf("init pthread attributes failed\n");
                goto out;
        }
 
        /* Set a specific stack size  */
        ret = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
        if (ret) {
        	printf("pthread setstacksize failed\n");
		goto out;
        }
 
        /* Set scheduler policy and priority of pthread */
        ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        if (ret) {
                printf("pthread setschedpolicy failed\n");
                goto out;
        }
        param.sched_priority = 80;
        ret = pthread_attr_setschedparam(&attr, &param);
        if (ret) {
                printf("pthread setschedparam failed\n");
                goto out;
        }
        /* Use scheduling parameters of attr */
        ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        if (ret) {
                printf("pthread setinheritsched failed\n");
                goto out;
        }
 
        /* Create a pthread with specified attributes */
        ret = pthread_create(&thread, &attr, simple_cyclic_task, NULL);
        if (ret) {
                printf("create pthread failed\n");
                goto out;
        }

	/* wait for rt_thread to start */
	while (!rt_thread_started);

        /* Join the thread and wait until it is done */
        ret = pthread_join(thread, NULL);
        if (ret)
                printf("join pthread failed: %m\n");

	printf("total_step_count = %d\n", total_step_count);
 
out:
	// Clear permission bits of 4 ports starting from BASE
	ioperm(BASE, 4, 0);
        return ret;
}


