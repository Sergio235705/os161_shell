/* * * * * * * * * * * * * * * *
* Sys Call file for LAB2 PDS
*
*
*
*
*
*
* * * * * * * * * * * * * * * */

#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <current.h>
#include <lib.h>

#include <copyinout.h>
#include <vnode.h>
#include <vfs.h>

#include <synch.h>
#include <uio.h>
#include <proc.h>
#include <kern/fcntl.h>

#if OPT_SHELL
#include <kern/stat.h>


/* system open file table */

struct tableOpenFile TabFile;

void openfileIncrRefCount(struct openfile *of)
{
  int i;
  lock_acquire(TabFile.lk);
  if (of != NULL)
  {
    for (i = 0; i < SYSTEM_OPEN_MAX; i++)
    {
      if (TabFile.systemFileTable[i].vn != NULL && TabFile.systemFileTable[i].vn == of->vn)
      {
        TabFile.systemFileTable[i].countRef++;
      }
    }
  }
  lock_release(TabFile.lk);
}

void openfileDecrRefCount(struct openfile *of)
{
  int i;
  if (of != NULL)
  {
    for (i = 0; i < SYSTEM_OPEN_MAX; i++)
    {
      if (TabFile.systemFileTable[i].vn != NULL && TabFile.systemFileTable[i].vn == of->vn)
      {
        TabFile.systemFileTable[i].countRef--;
      }
    }
  }
}

static int
file_read(int fd, userptr_t buf_ptr, size_t size)
{
  lock_acquire(TabFile.lk);
  struct iovec iov;
  struct uio ku;
  int offset;
  int result, nread;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd > OPEN_MAX)
    return -1;
  {
    of = curproc->fileTable[fd].of;
    offset = curproc->fileTable[fd].offset;
  }
  if (of == NULL)
    return -1;
  vn = of->vn;
  if (vn == NULL)
    return -1;

  kbuf = kmalloc(size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_READ);
  result = VOP_READ(vn, &ku);
  if (result)
  {
    return result;
  }
  curproc->fileTable[fd].offset = ku.uio_offset;
  nread = size - ku.uio_resid;
  copyout(kbuf, buf_ptr, nread);
  kfree(kbuf);
  lock_release(TabFile.lk);
  return (nread);
}

static int
file_write(int fd, userptr_t buf_ptr, size_t size)
{
  lock_acquire(TabFile.lk);

  struct iovec iov;
  struct uio ku;
  int offset;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd > OPEN_MAX)
    return -1;
  {
    of = curproc->fileTable[fd].of;
    offset = curproc->fileTable[fd].offset;
  }
  if (of == NULL)
    return -1;
  vn = of->vn;
  if (vn == NULL)
    return -1;

  kbuf = kmalloc(size);
  copyin(buf_ptr, kbuf, size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result)
  {
    lock_release(TabFile.lk);
    return result;
  }
  kfree(kbuf);
  curproc->fileTable[fd].offset = ku.uio_offset;
  nwrite = size - ku.uio_resid;
  lock_release(TabFile.lk);
  return (nwrite);
}

int sys__getcwd(char *buf, size_t buflen)
{
  struct iovec iov;
  struct uio ku;
  int result;

  uio_kinit(&iov, &ku, buf, buflen, 0, UIO_READ);
  result = vfs_getcwd(&ku);
  return result;
}

int sys_open(userptr_t path, int openflags, mode_t mode, int *errp)
{
  lock_acquire(TabFile.lk);

  int fd, i, offset;
  struct vnode *v;
  struct openfile *of = NULL;
  int result;

  result = vfs_open((char *)path, openflags, mode, &v);
  if (result)
  {
    *errp = ENOENT;
    lock_release(TabFile.lk);
    return -1;
  }
  /* search system open file table */
  for (i = 0; i < SYSTEM_OPEN_MAX; i++)
  {

    if (TabFile.systemFileTable[i].vn != NULL && TabFile.systemFileTable[i].vn == v)
    {
      of = &TabFile.systemFileTable[i];
      TabFile.systemFileTable[i].countRef++;
      if (openflags & O_APPEND)
      {
        for (fd = STDERR_FILENO + 1; fd < OPEN_MAX; fd++)
        {
          if (curproc->fileTable[fd].of == of)
          {
            offset = curproc->fileTable[fd].offset;
          }
        }
      }
      else
      {
        offset = 0;
      }
      break;
    }
  }
  if (of == NULL)
  {
    for (i = 0; i < SYSTEM_OPEN_MAX; i++)
    {
      if (TabFile.systemFileTable[i].vn == NULL)
      {
        of = &TabFile.systemFileTable[i];
        of->vn = v;
        of->countRef = 1; // ref;
        break;
      }
    }
  }

  if (of == NULL)
  {
    // no free slot in system open file table
    *errp = ENFILE;
  }
  else
  {
    for (fd = STDERR_FILENO + 1; fd < OPEN_MAX; fd++)
    {
      if (curproc->fileTable[fd].fd == -1)
      {

        curproc->fileTable[fd].of = of;
        curproc->fileTable[fd].offset = offset; //offset;// caso append
        curproc->fileTable[fd].flags = openflags;
        curproc->fileTable[fd].fd = fd;
        lock_release(TabFile.lk);
        return fd;
      }
    }
    // no free slot in process open file table
    *errp = EMFILE;
  }

  vfs_close(v);
  lock_release(TabFile.lk);
  return -1;
}

int 
sys_dup2(int oldfd, int newfd)
{
  sys_close(newfd);
  openfileIncrRefCount(curproc->fileTable[oldfd].of);
  curproc->fileTable[newfd] = curproc->fileTable[oldfd];
  /*cosa ritora questa funzione?*/
  return 0;
}

int 
sys_close(int fd)
{
  lock_acquire(TabFile.lk);

  struct openfile *of = NULL;
  struct vnode *vn;

  if (fd < 0 || fd > OPEN_MAX)
    return -1;
  of = curproc->fileTable[fd].of;
  /*curproc->fileTable.fd?*/
  if (of == NULL || curproc->fileTable[fd].fd == -1)
    return -1;
  curproc->fileTable[fd].of = NULL;
  curproc->fileTable[fd].fd = -1;

  openfileDecrRefCount(of);
  if (of->countRef > 0)
    return 0; // just decrement ref cnt

  vn = of->vn;
  of->vn = NULL;
  if (vn == NULL)
    return -1;

  vfs_close(vn);
  lock_release(TabFile.lk);
  return 0;
}

int 
sys_lseek(int fd, off_t offset, int start)
{
  struct stat *stats = NULL;
  if (start & SEEK_SET)
  {
    curproc->fileTable[fd].offset = offset;
    return 0;
  }
  if (start & SEEK_CUR)
  {
    curproc->fileTable[fd].offset += offset;
    return 0;
  }
  if (start & SEEK_END)
  {
    VOP_STAT(curproc->fileTable[fd].of->vn, stats);
    curproc->fileTable[fd].offset = stats->st_size + offset;
    return 0;
  }
  return -1;
}

/*
 * simple file system calls for write/read
 */
int sys_write(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
  {
    return file_write(fd, buf_ptr, size);
  }

  for (i = 0; i < (int)size; i++)
  {
    putch(p[i]);
  }

  return (int)size;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd != STDIN_FILENO)
  {
    return file_read(fd, buf_ptr, size);
  }

  for (i = 0; i < (int)size; i++)
  {
    p[i] = getch();
    if (p[i] < 0)
      return i;
  }

  return (int)size;
}

int sys_remove(userptr_t pathname, int32_t *retval)
{
	char name[NAME_MAX+1];
	size_t len;
  //From user ptr to kernel space
	if (copyinstr(pathname, name, NAME_MAX, &len) != 0) {
		*retval = -1; //TO DO: error managment
		return 1;
	}

	*retval = vfs_remove(name);
	if (*retval < 0) {
		*retval = -1;
		return 1; //TO DO: error managment
	}
	return 0;
}
#endif
