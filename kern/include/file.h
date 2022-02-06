/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_


/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>


/*
 * Put your function declarations and data types here ...
 */

struct open_file {
    off_t           offset;
    struct vnode   *v_ptr;
    int             flag;
    struct lock    *of_lock;
    int             ref_counter;
};

struct openfile_table {
    struct open_file   **of;
    struct lock        *of_table_lock;
};

struct openfile_table of_table;

void init_of_table(void);

int any_open(char *filename, int flags, mode_t mode, int *retval);

//int sys_open(const userptr_t filename, int flags, mode_t mode);

#endif /* _FILE_H_ */
