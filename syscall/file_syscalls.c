

#include <types.h>
#include <kern/stattypes.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <current.h>
#include <lib.h>
#include <vm.h>

#include <copyinout.h>
#include <vnode.h>
#include <vfs.h>

#include <synch.h>
#include <uio.h>
#include <kern/fcntl.h>

#if OPT_SHELL
#include <kern/stat.h>
#include <kern/seek.h>

/* system open file table */

struct tableOpenFile TabFile;

int sys__getcwd(char *buf, size_t buflen, int32_t *retval)
{
  struct iovec iov;
  struct uio ku;

  uio_kinit(&iov, &ku, buf, buflen, 0, UIO_READ);
  *retval = vfs_getcwd(&ku);
  return *retval;
}

int sys_open(struct proc *p, userptr_t path, int openflags, mode_t mode, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  int result, fd, i, offset = 0;
  struct vnode *v = NULL;
  struct stat *stats = NULL;
  struct openfile *of = NULL;

  if (path == NULL || (int *)path == (int *)0x40000000 || (int *)path == (int *)0x80000000)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EFAULT;
  }

  if (p == NULL)
  {
    p = curproc;
    if ((openflags & O_ACCMODE) >= 3 || (openflags & 0xFFFFFF80) != 0)
    {
      *retval = -1;
      lock_release(TabFile.lk);
      return EINVAL;
    }
  }

  result = vfs_open((char *)path, openflags, mode, &v);

  if (result)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return result;
  }

  /* search system open file table */
  for (i = 0; i < SYSTEM_OPEN_MAX; i++)
  {

    if (TabFile.systemFileTable[i].vn == v)
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
    *retval = -1;
    vfs_close(v);
    lock_release(TabFile.lk);
    return ENFILE;
  }
  else
  {
    for (fd = 0; fd < OPEN_MAX; fd++)
    {
      if (p->fileTable[fd] == NULL)
      {
        p->fileTable[fd] = kmalloc(sizeof(struct fileTableEntry));
        p->fileTable[fd]->of = of;
        p->fileTable[fd]->offset = offset;
        p->fileTable[fd]->flags = openflags;
        p->fileTable[fd]->fd = fd;
        p->fileTable[fd]->fteCnt = 1;
        *retval = fd;
        lock_release(TabFile.lk);
        return 0;
      }
    }
    *retval = -1;
    vfs_close(v);
    lock_release(TabFile.lk);
    return EMFILE;
  }
}

int sys_dup2(int oldfd, int newfd, int32_t *retval)
{
  struct fileTableEntry *fte = NULL;
  int result;

  lock_acquire(TabFile.lk);

  if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF; //fd is not a valid file handler
  }

  if (curproc->fileTable[newfd] != NULL)
  {
    if (oldfd == newfd)
    {
      *retval = oldfd;
      lock_release(TabFile.lk);
      return 0;
    }

    lock_release(TabFile.lk);
    result = sys_close(newfd, retval);
    if (result)
    {
      return result;
    }
    lock_acquire(TabFile.lk);
  }

  fte = curproc->fileTable[oldfd];
  if (fte == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  fte->fteCnt++;
  curproc->fileTable[newfd] = fte;

  *retval = newfd;
  lock_release(TabFile.lk);
  return 0;
}

int sys_close(int fd, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  struct openfile *of = NULL;
  struct fileTableEntry *fte = NULL;
  struct vnode *vn;

  if (fd < 0 || fd >= OPEN_MAX)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF; //fd is not a valid file handler
  }

  fte = curproc->fileTable[fd];
  if (fte == NULL)
  {
    lock_release(TabFile.lk);
    *retval = -1;
    return EBADF;
  }

  of = curproc->fileTable[fd]->of;
  curproc->fileTable[fd] = NULL;

  if (fte == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  /*if (fte->fd == STDIN_FILENO || fte->fd == STDOUT_FILENO || fte->fd == STDERR_FILENO)
  {
    lock_release(TabFile.lk);
    return 0;
  }*/

  fte->fteCnt--;
  if (fte->fteCnt == 0)
  {
    kfree(fte);
    of->countRef--;
    if (of->countRef == 0)
    {
      vn = of->vn;
      of->vn = NULL;
      vfs_close(vn);
      lock_release(TabFile.lk);
      return 0;
    }
  }

  lock_release(TabFile.lk);
  return 0;
}

int sys_lseek(int fd, off_t pos, int whence, int64_t *retval)
{
  lock_acquire(TabFile.lk);
  struct stat stats;
  mode_t result;

  if (fd < 0 || fd >= OPEN_MAX)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (curproc->fileTable[fd] == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  VOP_GETTYPE(curproc->fileTable[fd]->of->vn, &result);
  if (result == _S_IFCHR || result == _S_IFBLK)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return ESPIPE;
  }

  if (whence == SEEK_SET)
  {
    if (pos < 0)
    {
      *retval = -1;
      lock_release(TabFile.lk);
      return EINVAL;
    }
    curproc->fileTable[fd]->offset = pos;
    *retval = curproc->fileTable[fd]->offset;
    lock_release(TabFile.lk);
    return 0;
  }
  if (whence == SEEK_CUR)
  {
    if ((curproc->fileTable[fd]->offset + pos) < 0)
    {
      *retval = -1;
      lock_release(TabFile.lk);
      return EINVAL;
    }
    curproc->fileTable[fd]->offset += pos;
    *retval = curproc->fileTable[fd]->offset;
    lock_release(TabFile.lk);
    return 0;
  }
  if (whence == SEEK_END)
  {
    VOP_STAT(curproc->fileTable[fd]->of->vn, &stats);
    if ((stats.st_size + pos) < 0)
    {
      *retval = -1;
      lock_release(TabFile.lk);
      return EINVAL;
    }
    curproc->fileTable[fd]->offset = stats.st_size + pos;
    *retval = curproc->fileTable[fd]->offset;
    lock_release(TabFile.lk);
    return 0;
  }
  *retval = -1;
  lock_release(TabFile.lk);
  return EINVAL;
}

int sys_write(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{
  lock_acquire(TabFile.lk);

  struct iovec iov;
  struct uio ku;
  off_t offset;
  struct stat *stats = NULL;
  int nwrite, result;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd >= OPEN_MAX)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (curproc->fileTable[fd] == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (buf_ptr == NULL || (int *)buf_ptr == (int *)0x40000000 || (int *)buf_ptr == (int *)0x80000000)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EFAULT;
  }

  of = curproc->fileTable[fd]->of;
  if (of == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if ((curproc->fileTable[fd]->flags & O_ACCMODE) == O_RDONLY)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (curproc->fileTable[fd]->flags & O_APPEND)
  {
    VOP_STAT(of->vn, stats);
    offset = stats->st_size;
  }
  else
  {
    offset = curproc->fileTable[fd]->offset;
  }
  vn = of->vn;
  if (vn == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }
  kbuf = kmalloc(size);
  copyin(buf_ptr, kbuf, size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return result;
  }
  kfree(kbuf);
  curproc->fileTable[fd]->offset = ku.uio_offset;
  nwrite = size - ku.uio_resid;
  lock_release(TabFile.lk);
  *retval = nwrite;
  return 0;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size, int32_t *retval)
{

  lock_acquire(TabFile.lk);
  struct iovec iov;
  struct uio ku;
  off_t offset;
  int nread, result;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd < 0 || fd >= OPEN_MAX)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (curproc->fileTable[fd] == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if (buf_ptr == NULL || (int *)buf_ptr == (int *)0x40000000 || (int *)buf_ptr == (int *)0x80000000)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EFAULT;
  }
  of = curproc->fileTable[fd]->of;
  offset = curproc->fileTable[fd]->offset;

  if (of == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  if ((curproc->fileTable[fd]->flags & O_ACCMODE) == O_WRONLY)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  vn = of->vn;
  if (vn == NULL)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return EBADF;
  }

  kbuf = kmalloc(size);
  uio_kinit(&iov, &ku, kbuf, size, offset, UIO_READ);
  result = VOP_READ(vn, &ku);
  if (result)
  {
    *retval = -1;
    lock_release(TabFile.lk);
    return result;
  }
  curproc->fileTable[fd]->offset = ku.uio_offset;
  nread = size - ku.uio_resid;
  copyout(kbuf, buf_ptr, nread);
  kfree(kbuf);
  *retval = nread;
  lock_release(TabFile.lk);
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