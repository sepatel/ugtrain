/* libmemhack.c:    hacking of an unique malloc call (used by ugtrain)
 *
 * Copyright (c) 2013, by:      Sebastian Riemer
 *    All rights reserved.      Ernst-Reinke-Str. 23
 *                              10369 Berlin, Germany
 *                             <sebastian.riemer@gmx.de>
 *
 * powered by the Open Game Cheating Association
 *
 * This file may be used subject to the terms and conditions of the
 * GNU General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <dlfcn.h>      /* dlsym */
#include <stdio.h>      /* printf */
#include <stdlib.h>     /* malloc */
#include <string.h>
#include <fcntl.h>
#include <signal.h>     /* sigignore */
#include <unistd.h>     /* read */
#include <limits.h>     /* PIPE_BUF */

#define PFX "[memhack] "
#define OW_MALLOC 1
#define OW_FREE 1
#define BUF_SIZE PIPE_BUF
#define DYNMEM_IN  "/tmp/memhack_in"
#define DYNMEM_OUT "/tmp/memhack_out"
#define PTR_INVAL (void *) 1

#define DEBUG 0
#if !DEBUG
	#define printf(...) do { } while (0);
#endif

typedef unsigned long long u64;
typedef unsigned int u32;

/* get the stack pointer (x86 only) */
#ifdef __i386__
register void *reg_sp __asm__ ("esp");
typedef u32 ptr_t;
#else
register void *reg_sp __asm__ ("rsp");
typedef u64 ptr_t;
#endif

#define PTR_ADD(type, x, y)  (type) ((ptr_t)x + (ptr_t)y)
#define PTR_ADD2(type, x, y, z)  (type) ((ptr_t)x + (ptr_t)y + (ptr_t)z)
#define PTR_SUB(type, x, y)  (type) ((ptr_t)x - (ptr_t)y)

/* Config structure */
struct cfg {
	size_t mem_size;
	void *code_addr;
	void *stack_offs;
	u32 max_obj;
	void **mem_addrs;  /* filled by malloc for free */
};
typedef struct cfg cfg_s;


/* cfg_s pointers followed by cfg structs */
cfg_s *config[PIPE_BUF/sizeof(cfg_s *)] = { NULL };

/* File descriptors and output buffer */
static int ofd = -1, ifd = -1;
static char obuf[BUF_SIZE];

/* Output control */
static int active = 0;

/* Stack check */
/*
 * This is a global variable set at program start time. It marks the
 * highest used stack address.
 */
extern void *__libc_stack_end;


#define SET_IBUF_OFFS(count, i) \
	for (i = 0; i < count; ibuf_offs++) { \
		if (ibuf[ibuf_offs] == ';') \
			i++; \
	}

/* prepare memory hacking upon library load */
void __attribute ((constructor)) memhack_init (void)
{
	ssize_t rbytes;
	char ibuf[BUF_SIZE] = { 0 };
	int i, j, k, ibuf_offs = 0, num_cfg = 0, cfg_offs = 0, scanned = 0;
	u32 max_obj;
	int read_tries;

	sigignore(SIGPIPE);
	sigignore(SIGCHLD);

	ifd = open(DYNMEM_IN, O_RDONLY | O_NONBLOCK);
	if (ifd < 0) {
		perror(PFX "open input");
		exit(1);
	}
	printf(PFX "ifd: %d\n", ifd);

	printf(PFX "Waiting for output FIFO opener..\n");
	ofd = open(DYNMEM_OUT, O_WRONLY | O_TRUNC);
	if (ofd < 0) {
		perror(PFX "open output");
		exit(1);
	}
	printf(PFX "ofd: %d\n", ofd);

	for (read_tries = 5; ; --read_tries) {
		rbytes = read(ifd, ibuf, sizeof(ibuf));
		if (rbytes > 0)
			break;
		if (read_tries <= 0)
			goto read_err;
		usleep(250 * 1000);
	}

	scanned = sscanf(ibuf + ibuf_offs, "%d", &num_cfg);
	if (scanned != 1)
		goto err;
	SET_IBUF_OFFS(1, j);
	cfg_offs = (num_cfg + 1) * sizeof(cfg_s *);
	max_obj = sizeof(config) - cfg_offs - num_cfg * sizeof(cfg_s);
	max_obj /= sizeof(void *);
	if (max_obj <= 1)
		fprintf(stderr, PFX "Error: No space for memory addresses!\n");
	else
		fprintf(stdout, PFX "Using max. %u objects per class.\n",
			max_obj - 1);

	/* read config into config array */
	for (i = 0; i < num_cfg; i++) {
		config[i] = PTR_ADD2(cfg_s *, config, cfg_offs,
			i * sizeof(cfg_s));
		if ((ptr_t) config[i] + sizeof(cfg_s) - (ptr_t) config
		    > PIPE_BUF || ibuf_offs >= BUF_SIZE) {
			/* config doesn't fit, truncate it */
			fprintf(stderr, PFX "Config buffer too "
					"small, truncating!\n");
			config[i] = NULL;
			num_cfg = i;
			break;
		}
		scanned = sscanf(ibuf + ibuf_offs, "%zd;%p;%p",
			&config[i]->mem_size, &config[i]->code_addr,
			&config[i]->stack_offs);
		if (scanned != 3)
			goto err;
		SET_IBUF_OFFS(3, j);

		if (max_obj <= 1)
			continue;
		/* put stored memory addresses behind all cfg_s stuctures */
		config[i]->max_obj = max_obj - 1;
		config[i]->mem_addrs = PTR_ADD2(void **, config, cfg_offs,
			num_cfg * sizeof(cfg_s));
		config[i]->mem_addrs = PTR_ADD(void **, config[i]->mem_addrs,
			i * max_obj);
		/* fill with invalid pointers to detect end */
		for (k = 0; k < max_obj; k++)
			config[i]->mem_addrs[k] = PTR_INVAL;
	}
	if (i != num_cfg)
		goto err;

	/* debug config */
	printf(PFX "num_cfg: %d, cfg_offs: %d\n", num_cfg, cfg_offs);
	for (i = 0; config[i] != NULL; i++) {
		fprintf(stdout, PFX "config[%d]: mem_size: %zd; "
			"code_addr: %p; stack_offs: %p\n", i,
			config[i]->mem_size, config[i]->code_addr,
			config[i]->stack_offs);
	}

	if (num_cfg > 0)
		active = 1;
	return;

read_err:
	fprintf(stderr, PFX "Can't read config, disabling output.\n");
	return;
err:
	fprintf(stderr, PFX "Error while reading config!\n");
	return;
}

/* void *malloc (size_t size); */
/* void *calloc (size_t nmemb, size_t size); */
/* void *realloc (void *ptr, size_t size); */
/* void free (void *ptr); */

#ifdef OW_MALLOC
void *malloc (size_t size)
{
	void *mem_addr, *stack_addr;
	int i, j, wbytes;
	static void *(*orig_malloc)(size_t size) = NULL;

	/* get the libc malloc function */
	if (!orig_malloc)
		orig_malloc = (void *(*)(size_t)) dlsym(RTLD_NEXT, "malloc");

	mem_addr = orig_malloc(size);

	if (active) {
		for (i = 0; config[i] != NULL; i++) {
			if (size != config[i]->mem_size)
				continue;

			stack_addr = PTR_SUB(void *, __libc_stack_end,
				config[i]->stack_offs);

			if (reg_sp > stack_addr ||
			    *(ptr_t *)stack_addr !=
			    (ptr_t) config[i]->code_addr)
				continue;

			printf(PFX "malloc: mem_addr: %p, code_addr: %p\n",
				mem_addr, config[i]->code_addr);
			wbytes = sprintf(obuf, "m%p;c%p\n",
				mem_addr, config[i]->code_addr);

			wbytes = write(ofd, obuf, wbytes);
			if (wbytes < 0) {
				perror("write");
			} else if (config[i]->mem_addrs) {
				for (j = 0; j < config[i]->max_obj; j++) {
					if (config[i]->mem_addrs[j] <= PTR_INVAL) {
						config[i]->mem_addrs[j] = mem_addr;
						break;
					}
				}
			}
			break;
		}
	}
	return mem_addr;
}
#endif


#ifdef OW_FREE
void free (void *ptr)
{
	int i, j, wbytes;
	static void (*orig_free)(void *ptr) = NULL;

	if (active && ptr != NULL) {
		for (i = 0; config[i] != NULL; i++) {
			if (!config[i]->mem_addrs)
				continue;
			for (j = 0; config[i]->mem_addrs[j] != PTR_INVAL; j++) {
				if (config[i]->mem_addrs[j] == ptr)
					goto found;
			}
			continue;
found:
			printf(PFX "free: mem_addr: %p\n", ptr);
			wbytes = sprintf(obuf, "f%p\n", ptr);

			wbytes = write(ofd, obuf, wbytes);
			if (wbytes < 0)
				perror("write");
			config[i]->mem_addrs[j] = NULL;
			break;
		}
	}
	/* get the libc free function */
	if (!orig_free)
		orig_free = (void (*)(void *)) dlsym(RTLD_NEXT, "free");

	orig_free(ptr);
}
#endif
