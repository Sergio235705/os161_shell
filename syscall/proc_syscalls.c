/* * * * * * * * * * * * * * * * * * * * * 
* Syscall file for LAB2 PDS
* This file contain the "exit()" syscall
* used to terminate a process.
*
*
*
*
* * * * * * * * * * * * * * * * * * * * */

#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <lib.h>
#include <syscall.h>
#include <proc.h>
#include <clock.h>
#include <copyinout.h>
#include <thread.h>
#include <kern/fcntl.h>
#include <vm.h>
#include <vfs.h>
#include <addrspace.h>
#include <current.h>
#include <cdefs.h> /* for __DEAD */
#include <mips/trapframe.h>
#include <synch.h>
#include <test.h>
#include <kern/wait.h>
#if OPT_SHELL


void
sys__exit(int status)
{
	struct proc *p = curproc;
	/*Let's create the error code in a linux fashion:*/
	p->p_status = _MKWAIT_EXIT(status);
	proc_remthread(curthread);
	V(p->p_sem);
	thread_exit();
	panic("thread_exit returned\n");

}




int
sys_waitpid(pid_t pid, userptr_t statusp, int options,pid_t* retval)
{
	*retval = -1;
	/*Here we assume that pid can be only positive: no concept of group id*/
	if (pid <= 0){
		return ENOSYS;
	}
	if(pid >= PID_MAX || pid >= MAX_PROC || pid == curproc->p_pid || pid == curproc->parent_p_pid)
		return ECHILD;
	struct proc *p = proc_search_pid(pid);
	int s,s1;
	if(options!=0) //TO DO: Gestire le opzioni
		if(options!= WNOHANG && options != WUNTRACED && options != (WNOHANG||WUNTRACED) )
			return EINVAL;
	if (p==NULL){
		return ESRCH;
	}
	s = proc_wait(p);

	if (statusp!=NULL)
	{
	int result = copyout(&s, (userptr_t) statusp, sizeof(int));

	if(result)
		return result;
	if(copyin((userptr_t) statusp,&s1, sizeof(int)))
		kprintf("bruh...2\n");

	//memcpy(ret, &pid, sizeof(int));
	}
	*retval = pid;
	return 0;
}

pid_t
sys_getpid(void)
{
  KASSERT(curproc != NULL);
  return curproc->p_pid;
}

static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
 
  panic("enter_forked_process returned (should not happen)\n");
}

int sys_fork(struct trapframe *ctf,pid_t* retval) {
	struct trapframe *tf_child;
	struct proc *newp;
	int result;

	KASSERT(curproc != NULL);

	newp = proc_create_runprogram(curproc->p_name);
	if (newp == NULL) {
		return ENOMEM;
	}
	as_copy(curproc->p_addrspace, &(newp->p_addrspace));
	if(newp->p_addrspace == NULL){
		proc_destroy(newp); 
		return ENOMEM; 
	}

	proc_file_table_copy(curproc, newp);
	tf_child = kmalloc(sizeof(struct trapframe));
	if(tf_child == NULL){
		proc_destroy(newp);
		return ENOMEM; 
	}
	memcpy(tf_child, ctf, sizeof(struct trapframe));

	/* TO BE DONE: linking parent/child, so that child terminated 
		on parent exit */
	newp->parent_p_pid = curproc->p_pid;
	result = thread_fork(
			curthread->t_name, newp,
			call_enter_forked_process, 
			(void *)tf_child, (unsigned long)0/*unused*/);

	if (result){
		proc_destroy(newp);
		kfree(tf_child);
		return ENOMEM;
  }

 
	*retval = newp->p_pid;
  return  0;
}

int sys_execv(char *progname, char *args[]){
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	int argc=0;
	int i = 0,len,j;
	struct addrspace * old_as;
	char* garbage = kmalloc( (ARG_MAX+1) *sizeof(char));
	char* path_name = kmalloc( (PATH_MAX+1) *sizeof(char));
	size_t size = 0;
	int end = 0;
	result = copyinstr((userptr_t)progname, path_name, PATH_MAX, &size);
	if(result)
		return result;	
	char** argv = (char **) kmalloc(sizeof (args) * sizeof (char *));
	if (!argv) {
	   return -1;
	}
	/*if (args == NULL)
		argc = 0;
	else*/
	//Riga aggiunta per committare
	i = 0;
	{
		result = copyinstr((userptr_t)args, garbage, ARG_MAX, &size);
		if (result)
			return result;
		while (!end)
		{
			size = 0;
			if(args[i] == NULL){
				break;
			}
			result = copyinstr((userptr_t)args[i], garbage, ARG_MAX, &size);
				if (result)
					return result;
			i++;
			if(size == 0)
				end = 1;


		}
		argc = i;
	}
		
	// looping through the arguments to copy into the new array.
	for (i = 0;i < argc; i++) {

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


	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	KASSERT(proc_getas() != NULL);
	old_as = curproc->p_addrspace;
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
		proc_setas(old_as);
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
	as_destroy(old_as);
	

	//Copy arguments from the kernel to the users stack.
	size_t actual;
	vaddr_t topstack[argc];
	// Looping through the array to copy the arguments into the stack.
	// The stack is bottom up, so we start with the last argument of argv
	for (i = argc - 1; i >= 0; i--) {
	len = strlen(argv[i]) + 4 - (strlen(argv[i]) % 4); //modified here
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
	kfree(path_name);
	kfree(garbage);
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








#endif