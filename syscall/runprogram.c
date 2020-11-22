/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */

#if OPT_SHELL
int
runprogram(char *progname,char** args,int argc)
{ 
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

		/* Passing argument */
	int len, i,j;
	char** argv = (char **) kmalloc(sizeof (args) * sizeof (char *));
	if (!argv) {
	   return -1;
	}

	// looping through the arguments to copy into the new array.
	for (i = 0; i < argc; i++) {

		len = strlen(args[i]) + 4 - (strlen(args[i]) % 4);
		argv[i] = (char *) kmalloc(len);
		if (!argv[i]) {
			panic("Out of memory copying argument\n");
		}

		// Put '\0' in each of the spots.
		for (j = 0; j < len; j++)
			argv[i][j] = '\0';
		// copy the arguments into argv and free them from args.
		memcpy(argv[i], args[i], strlen(args[i]));
	}

	//Copy arguments from the kernel to the users stack.
	size_t actual;
	vaddr_t topstack[argc];
	// Looping through the array to copy the arguments into the stack.
	// The stack is bottom up, so we start with the last argument of argv
	for (i = argc - 1; i >= 0; i--) {
		len = strlen(argv[i]) + 4 - (strlen(argv[i]) % 4);
		// Decrement the stack pointer to copy in the arguments.
		stackptr -= len;
		// copy the arguments into the stack and free them from argv.
		result = copyoutstr(argv[i], (userptr_t) stackptr, len, &actual);
		if(result != 0){
			kprintf("copyoutstr failed: %s\n", strerror(result));
			return result;
		}
	kfree(argv[i]);
	// save the stack address of the arguments in the original order.
	topstack[argc - i - 1] = stackptr;
	}
	kfree(argv);

	// decrement the stack pointer and add 4 null bytes of padding.
	stackptr -= 4;
	char nullbytes[4];
	nullbytes[0] = nullbytes[1] = nullbytes[2] = nullbytes[3] = 0x00;
	result = copyoutstr(nullbytes, (userptr_t) stackptr, 4, &actual);
	if(result != 0){
	    kprintf("copyoutstr failed: %s\n", strerror(result));
	    return result;
	}

	// writing the addresses of the arguments in the stack, into the stack.
	for (i = 0; i < argc; i++) {
		stackptr -= 4;
		result = copyout(&topstack[i], (userptr_t) stackptr, sizeof (topstack[i]));
		if(result != 0){
			kprintf("copyout failed: %s\n", strerror(result));
			return result;
		}
	}
			

	/* Warp to user mode. */
	enter_new_process(argc /*argc*/, (userptr_t) stackptr/*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;

}

#else
int
runprogram(char *progname)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif
