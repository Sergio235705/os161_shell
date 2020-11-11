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
#if OPT_SHELL

void
sys__exit(int status , int32_t *retval)
{ 
  struct proc *p = curproc;
  *retval=status & 0xff;
  p->p_status = status & 0xff; /* just lower 8 bits returned */
  proc_remthread(curthread);
  V(p->p_sem);
  thread_exit();
  panic("thread_exit returned\n");

}




int
sys_waitpid(pid_t pid, userptr_t statusp, int options , int32_t *retval)
{
  struct proc *p = proc_search_pid(pid);
  int s;
  (void)options; /* not handled */
  if (p==NULL) {
	  *retval=-1;
	  return -1;}
  s = proc_wait(p);
  if (statusp!=NULL) 
    *(int*)statusp = s;
  return pid;
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

pid_t sys_fork(struct trapframe *ctf , int32_t *retval){
  struct trapframe *tf_child;
  struct proc *newp;
  int result;

  KASSERT(curproc != NULL);

  newp = proc_create_runprogram(curproc->p_name);
  if (newp == NULL) {

	    *retval=ENOMEM;
	  return -1;}
   
  
  as_copy(curproc->p_addrspace, &(newp->p_addrspace));
  if(newp->p_addrspace == NULL){
    proc_destroy(newp); 
      *retval=ENOMEM;
	  return -1;}
  tf_child = kmalloc(sizeof(struct trapframe));
  if(tf_child == NULL){
    proc_destroy(newp);
       *retval=ENOMEM;
	  return -1;}
  memcpy(tf_child, ctf, sizeof(struct trapframe));

  /* TO BE DONE: linking parent/child, so that child terminated 
     on parent exit */

  result = thread_fork(
		 curthread->t_name, newp,
		 call_enter_forked_process, 
		 (void *)tf_child, (unsigned long)0/*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
       *retval=ENOMEM;
	  return -1;}

 

  return  newp->p_pid;
}


int sys_execv(char *progname, char *args[], int32_t *retval)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	int argc=0;
	int i = 0,len,j;
	struct addrspace * old_as;
	char** argv = (char **) kmalloc(sizeof (args) * sizeof (char *));
	if (!argv) {
	     *retval=-1;
	  return -1;}
	// looping through the arguments to copy into the new array.
	for (i = 0;args[i] != NULL; i++) {

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

	argc = i;

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
		   *retval=ENOMEM;
	  return -1;}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		   *retval=result;
	  return -1;}
	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		   *retval=result;
	  return -1;}
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
	     *retval=result;
	  return -1;}
	kfree(argv[i]);
	// save the stack address of the arguments in the original order.
	topstack[argc - i - 1] = stackptr;
	}
	kfree(argv);

	// decrement the stack pointer and add 4 null bytes of padding.
	stackptr -= 4;
	copyoutstr(NULL, (userptr_t) stackptr, 4, &actual);
	if(result != 0){
	    kprintf("copyoutstr failed: %s\n", strerror(result));
	     *retval=result;
	  return -1;}
	// writing the addresses of the arguments in the stack, into the stack.
	for (i = 0; i < argc; i++) {
	stackptr -= 4;
	result = copyout(&topstack[i], (userptr_t) stackptr, sizeof (topstack[i]));
	if(result != 0){
	    kprintf("copyout failed: %s\n", strerror(result));
	     *retval=result;
	  return -1;}
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
