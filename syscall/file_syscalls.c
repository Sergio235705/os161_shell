

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
#include <kern/seek.h>

/* system open file table */

struct tableOpenFile TabFile;

static int
file_read(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{
  lock_acquire(TabFile.lk);
  struct iovec iov;
  struct uio ku;
  int offset;
  int nread;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd > OPEN_MAX)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }

  of = curproc->fileTable[fd].of;
  offset = curproc->fileTable[fd].offset;

  if (of == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }
  vn = of->vn;
  if (vn == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }

  kbuf = kmalloc(size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_READ);
  *retval = VOP_READ(vn, &ku);
  if (*retval)
  {
    lock_release(TabFile.lk);
    return *retval;
  }
  curproc->fileTable[fd].offset = ku.uio_offset;
  nread = size - ku.uio_resid;
  copyout(kbuf, buf_ptr, nread);
  kfree(kbuf);
  lock_release(TabFile.lk);
  *retval=nread;
  return 0;
}

static int
file_write(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  struct iovec iov;
  struct uio ku;
  int offset;
  struct stat *stats = NULL;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd > OPEN_MAX)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }

  of = curproc->fileTable[fd].of;
  if (curproc->fileTable[fd].flags & O_APPEND)
  {
    VOP_STAT(of->vn, stats);
    offset = stats->st_size;
  }
  else
  {
    offset = curproc->fileTable[fd].offset;
  }

  if (of == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }
  vn = of->vn;
  if (vn == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return 1;
  }
  kbuf = kmalloc(size);
  copyin(buf_ptr, kbuf, size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result)
  {
    lock_release(TabFile.lk);
    *retval=-1;
      return 1;} // Ritornare errore specifico
  kfree(kbuf);
  curproc->fileTable[fd].offset = ku.uio_offset;
  nwrite = size - ku.uio_resid;
  lock_release(TabFile.lk);
  *retval=nwrite;
  return 0;
}

int sys__getcwd(char *buf, size_t buflen, int32_t *retval)
{
  struct iovec iov;
  struct uio ku;

  uio_kinit(&iov, &ku, buf, buflen, 0, UIO_READ);
  *retval = vfs_getcwd(&ku);
  return 0;
}

int sys_open(userptr_t path, int openflags, mode_t mode, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  int result, fd, i, offset = 0;
  struct vnode *v;
  struct stat *stats = NULL;
  struct openfile *of = NULL;

  result = vfs_open((char *)path, openflags, mode, &v);

  if (result)
  {
    lock_release(TabFile.lk);
    return result;
  }

  /* search system open file table */
  for (i = 0; i < SYSTEM_OPEN_MAX; i++)
  {

    if (TabFile.systemFileTable[i].vn != NULL && TabFile.systemFileTable[i].vn == v)
    {
      of = &TabFile.systemFileTable[i];
      TabFile.systemFileTable[i].countRef++;
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

  if (openflags & O_APPEND)
  {
    VOP_STAT(of->vn, stats);
    offset = stats->st_size;
  }
  else
  {
    offset = 0;
  }

  if (of == NULL)
  {
    // no free slot in system open file table
    return ENFILE;
  }
  else
  {
    for (fd = STDERR_FILENO + 1; fd < OPEN_MAX; fd++)
    {
      if (curproc->fileTable[fd].fd == -1)
      {
        curproc->fileTable[fd].of = of;
        curproc->fileTable[fd].offset = offset;
        curproc->fileTable[fd].flags = openflags;
        curproc->fileTable[fd].fd = fd;
        curproc->fileTable[fd].dup[0] = fd;
        *retval = fd;
        lock_release(TabFile.lk);
        return 0;
      }
    }
    // no free slot in process open file table
    return EMFILE;
  }

  vfs_close(v);
  lock_release(TabFile.lk);
  return 0;
}

int sys_dup2(int oldfd, int newfd, int32_t *retval)
{
  int i;

  sys_close(newfd, 0);
  lock_acquire(TabFile.lk);
  curproc->fileTable[oldfd].of->countRef++;
  lock_release(TabFile.lk);
  updateDup(oldfd, newfd);
  curproc->fileTable[newfd] = curproc->fileTable[oldfd];
  for (i = 0; i < OPEN_MAX / 10; i++)
  {
    if (curproc->fileTable[oldfd].dup[i] != newfd && curproc->fileTable[oldfd].dup[i] != oldfd && curproc->fileTable[oldfd].dup[i] != -1)
      updateDup(curproc->fileTable[oldfd].dup[i], newfd);
  }
<<<<<<< HEAD
  *retval = 0;
  return newfd;
=======
  *retval=newfd;
  return 0;
>>>>>>> ce009212d71e5836efb699545753a6c7e06cfa17
}

void updateDup(int oldfd, int newfd)
{
  int i;
  for (i = 0; i < OPEN_MAX / 10; i++)
    if (curproc->fileTable[oldfd].dup[i] == -1)
    {
      curproc->fileTable[oldfd].dup[i] = newfd;
      break;
    }
}

int sys_close(int fd, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  struct openfile *of = NULL;
  struct vnode *vn;

  if (fd < 0 || fd > OPEN_MAX)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF; //fd is not a valid file handler
  }
  of = curproc->fileTable[fd].of;
  /*curproc->fileTable.fd?*/
  if (of == NULL || curproc->fileTable[fd].fd == -1)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF; 
  }
  curproc->fileTable[fd].of = NULL;
  curproc->fileTable[fd].fd = -1;

  of->countRef--;
  if (of->countRef > 0)
  {
    lock_release(TabFile.lk);
    *retval=0;
    return 0;
  } // just decrement ref cnt

  vn = of->vn;
  of->vn = NULL;
  of->offset = 0;
  if (vn == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF;
  }

  vfs_close(vn);
  *retval=0;
  lock_release(TabFile.lk);
  return 0;
}

int sys_lseek(int fd, off_t offset, int start, int32_t *retval)
{
  lock_acquire(TabFile.lk);
  int i, result;
  struct openfile *of = NULL;
  if(fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return ESPIPE;
  }
  if (fd < 0 || fd > OPEN_MAX)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }
  of = curproc->fileTable[fd].of;
  if (of == NULL || curproc->fileTable[fd].fd == -1)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF; 
  }
  for (i = 0; i < OPEN_MAX / 10; i++)
  {
    if (curproc->fileTable[fd].dup[i] != -1)
    {
      result = changeOffset(curproc->fileTable[fd].dup[i], offset, start);
      if (result)
      {
        *retval = -1;
        lock_release(TabFile.lk);
        return result;
      }
    }
  }
  *retval = curproc->fileTable[fd].offset;
  lock_release(TabFile.lk);
  return 0;
}

int changeOffset(int fd, off_t offset, int start)
{
  struct stat *stats = NULL;
  if (start & SEEK_SET)
  {
    if(offset < 0)
    {
      return EINVAL;
    }
    curproc->fileTable[fd].offset = offset;
    return 0;
  }
  if (start & SEEK_CUR)
  {
    if((curproc->fileTable[fd].offset + offset) < 0)
    {
      return EINVAL;
    }
    curproc->fileTable[fd].offset += offset;
    return 0;
  }
  if (start & SEEK_END)
  {
    VOP_STAT(curproc->fileTable[fd].of->vn, stats);
    if((stats->st_size + offset) < 0)
    {
      return EINVAL;
    }
    curproc->fileTable[fd].offset = stats->st_size + offset;
    return 0;
  }
  return EINVAL;
}

/*
 * simple file system calls for write/read
 */
int sys_write(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
  {
    return file_write(fd, buf_ptr, size, retval);
  }

  for (i = 0; i < (int)size; i++)
  {
    putch(p[i]);
  }
  *retval=(int)size;
  return 0;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd != STDIN_FILENO)
  {
    return file_read(fd, buf_ptr, size, retval);
  }

  for (i = 0; i < (int)size; i++)
  {
    p[i] = getch();
    if (p[i] < 0)
    {
      *retval = i;
      return i;
    }
  }

   *retval=(int)size;
  return 0;
}

int sys_remove(userptr_t pathname, int32_t *retval)
{
  char name[NAME_MAX + 1];
  size_t len;
  //From user ptr to kernel space
  if (copyinstr(pathname, name, NAME_MAX, &len) != 0)
  {
    *retval = -1; //TO DO: error managment
    return 1;
  }

  *retval = vfs_remove(name);
  if (*retval < 0)
  {
    *retval = -1;
    return 1; //TO DO: error managment
  }
  return 0;
}
#endif