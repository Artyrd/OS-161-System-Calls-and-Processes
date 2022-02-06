#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

/*
 * Add your file-related functions here ...
 */


void init_of_table(void) {
    of_table.of = kmalloc(__OPEN_MAX * sizeof(struct openfile *));
    for (int i = 0; i < __OPEN_MAX; i++) {
        of_table.of[i] = NULL;
    }
    of_table.of_table_lock = lock_create("of_table");
    if (of_table.of_table_lock == NULL) {
        kfree(of_table.of);
        // main has no return, panic
        panic("Failed to create lock for open file table");
    }

}


int sys_open(const userptr_t filename, int flags, mode_t mode, int *retval) {
    char new_filename[__PATH_MAX];
    size_t got;
    // check filepath
    copyinstr(filename, new_filename, sizeof(new_filename), &got);

    // safe to hand work to any_open
    return any_open(new_filename, flags, mode, retval);
}

int any_open(char *filename, int flags, mode_t mode, int *retval) {
    struct vnode *new_vnode;
    int vfs_result;
    struct open_file *new_of;
    int fd = -1;

    // check for room in fd table
    for (int i=0; i < __OPEN_MAX; i++) {
        if (curproc->fd_table[i] == -1) { 
            fd = i; // found available fd slot
            break;
        }
    }
    if (fd == -1) {
        return ENFILE; //fd table full
    }
    // check for valid flags
    int mode_chk = flags & O_ACCMODE;
    if (!(mode_chk == O_RDONLY || mode_chk == O_WRONLY || mode_chk == O_RDWR)) {
        return EINVAL;
    }
    // get vnode
    vfs_result = vfs_open(filename, flags, mode, &new_vnode);
    if (vfs_result) {
        return vfs_result;
    }
    // lock table
    lock_acquire(of_table.of_table_lock);

    // find available of_table slot
    int of_n = -1;
    for (int j=0; j < __OPEN_MAX; j++) {
        if (of_table.of[j] == NULL) {
            of_n = j;
            break;
        }
    }
    if (of_n == -1) {
        // release lock
        lock_release(of_table.of_table_lock);
        vfs_close(new_vnode);
        return ENFILE; // systemwide file table full
    }

    // create open_file
    new_of = kmalloc(sizeof(struct open_file));
    if (new_of == NULL) {
        lock_release(of_table.of_table_lock);
        vfs_close(new_vnode);
        return ENOMEM;
    }
    // initialise new open_file
    new_of->offset  = 0;
    new_of->v_ptr   = new_vnode;
    new_of->flag    = flags;
    new_of->of_lock = lock_create("of_lock");
    if (new_of->of_lock == NULL) {
        vfs_close(new_vnode);
        kfree(new_of);
        lock_release(of_table.of_table_lock);
        return ENOMEM;
    }
    new_of->ref_counter = 1;

    of_table.of[of_n] = new_of;

    // release lock
    lock_release(of_table.of_table_lock);

    // insert into fd
    curproc->fd_table[fd] = of_n;

    *retval = fd;

    return 0;

}


int sys_close(int fd) {
    struct open_file *target_of;
    struct vnode *vn;

    if (fd < 0 || fd >=  __OPEN_MAX) {
        return EBADF;
    }
    int of_n = curproc->fd_table[fd];
    if (of_n == -1) {
       return EBADF; 
    }
    // don't need to check if of_table.of[of_n] == NULL
    // because it only occurs for of_n == -1

    // obtain lock of openfile table before removing entry
    lock_acquire(of_table.of_table_lock);

    target_of = of_table.of[of_n];

    // obtain lock of target_of before closing
    lock_acquire(target_of->of_lock);

    if (target_of->ref_counter > 1) {
        // decrement open file reference counter if another process still using open_file
        target_of->ref_counter += -1;
        lock_release(target_of->of_lock);

    } else if (target_of->ref_counter == 1) {
        // destroy open file because no one still wants it
        vn = target_of->v_ptr;

        vfs_close(vn); // vfs_close handles closing the vnode incase another of is still referring to it

        // release & free openfile lock
        lock_release(target_of->of_lock);
        lock_destroy(target_of->of_lock);
        // free open file
        kfree(target_of);
        of_table.of[of_n] = NULL;
    }

    lock_release(of_table.of_table_lock);

    // invalidate file descriptor
    curproc->fd_table[fd] = -1;

    return 0;
}


int sys_read(int fd, void *buf, size_t buflen, int *retval) {
    struct iovec iov;
    struct uio uio;
    struct vnode *vn;
    int result = 0;

    // check invalid fd
    if (fd < 0 || fd >=  __OPEN_MAX) {
        return EBADF;
    }
    int of_n = curproc->fd_table[fd];
    if (of_n == -1) {
        return EBADF;
    }

    struct open_file *read_of = of_table.of[of_n];
    lock_acquire(read_of->of_lock);

    // check if file is readable
    int mode_chk = read_of->flag & O_ACCMODE;
    if (mode_chk == O_WRONLY) {
        lock_release(read_of->of_lock);
        return EBADF;
    }

    vn = read_of->v_ptr;

    // fill iov and uio for VOP_READ
    uio_uinit(&iov, &uio, buf, buflen, read_of->offset, UIO_READ);

    result = VOP_READ(vn, &uio);
    if (result) {
        lock_release(read_of->of_lock);
        return result;
    }

    // update new offset
    of_table.of[of_n]->offset = uio.uio_offset;

    *retval = buflen - uio.uio_resid;

    lock_release(read_of->of_lock);

    return 0;
}


int sys_write(int fd, userptr_t buf, size_t nbytes, int *retval) {
    struct iovec iov;
    struct uio uio;
    struct vnode *vn;
    int result = 0;

    // check invalid fd
    if (fd < 0 || fd >=  __OPEN_MAX) {
        return EBADF;
    }
    int of_n = curproc->fd_table[fd];
    if (of_n == -1) {
        return EBADF;
    }

    struct open_file *write_of = of_table.of[of_n];
    lock_acquire(write_of->of_lock);

    // check if file is writable
    int mode_chk = write_of->flag & O_ACCMODE;
    if (mode_chk == O_RDONLY) {
        lock_release(write_of->of_lock);
        return EBADF;
    }

    vn = write_of->v_ptr;

    // fill iov and uio for VOP_WRITE
    uio_uinit(&iov, &uio, buf, nbytes, write_of->offset, UIO_WRITE);

    result = VOP_WRITE(vn, &uio);
    if (result) {
        lock_release(write_of->of_lock);
        return result;
    }

    // update new offset
    of_table.of[of_n]->offset = uio.uio_offset;

    *retval = nbytes - uio.uio_resid;

    lock_release(write_of->of_lock);

    return 0;
}


int sys_lseek(int fd, off_t pos, int whence, off_t *retval64) {
    struct vnode *vn;
    struct stat info;
    off_t file_eof;
    int result = 0;

    // check invalid fd
    if (fd < 0 || fd >=  __OPEN_MAX) {
        return EBADF;
    }
    int of_n = curproc->fd_table[fd];
    if (of_n == -1) {
        return EBADF;
    }

    // obtain of table entry
    struct open_file *lseek_of = of_table.of[of_n];
    lock_acquire(lseek_of->of_lock);

    // obtain and check vnode if seekable
    vn = lseek_of->v_ptr;
    if (!VOP_ISSEEKABLE(vn)) {
        lock_release(lseek_of->of_lock);
        return ESPIPE;
    }

    result = VOP_STAT(vn, &info);
    if (result) {
        lock_release(lseek_of->of_lock);
        return result;
    }
    file_eof = info.st_size;

    // adjust offset accordingly, making sure it's not negative
    if (whence == SEEK_SET) {
        if (pos < 0) {
            lock_release(lseek_of->of_lock);
            return EINVAL;
        }
        lseek_of->offset = pos;
    } else if (whence == SEEK_CUR) {
        if (lseek_of->offset + pos < 0) {
            lock_release(lseek_of->of_lock);
            return EINVAL;
        }
        lseek_of->offset = lseek_of->offset + pos;
    } else if (whence == SEEK_END) {
        if (file_eof + pos < 0) {
            lock_release(lseek_of->of_lock);
            return EINVAL;
        }
        lseek_of->offset = file_eof + pos;
    } else {
        lock_release(lseek_of->of_lock);
        return EINVAL;
    }

    *retval64 = lseek_of->offset;

    lock_release(lseek_of->of_lock);

    return 0;
}


int sys_dup2(int old_fd, int new_fd, int *retval) {
    // check valid fd numbers
    if (old_fd < 0 || old_fd >=  __OPEN_MAX) 
        return EBADF;
    if (new_fd < 0 || new_fd >=  __OPEN_MAX) 
        return EBADF;

    // check old_fd refers to a valid open_file
    int of_n = curproc->fd_table[old_fd];
    if (of_n == -1) {
        return EBADF;
    }

    // if cloning a file handle onto itself, no efect
    if (old_fd == new_fd) {
        return 0;
    }

    // if new_fd refers to an already open_file, close it
    if (curproc->fd_table[new_fd] != -1) {
        int close_result = sys_close(new_fd);
        if (close_result) {
            return close_result;
        }
    }
    // lock target open_file
    lock_acquire(of_table.of[of_n]->of_lock);

    // make newfd refer to oldfd's openfile and increment ref_counter
    curproc->fd_table[new_fd] = of_n;
    of_table.of[of_n]->ref_counter += 1;

    // done with open_file
    lock_release(of_table.of[of_n]->of_lock);

    *retval = new_fd;

    return 0;
}
