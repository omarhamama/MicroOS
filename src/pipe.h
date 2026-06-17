#pragma once

struct pipe;
struct file;

/* Create a pipe and wrap its two ends in file objects:
 * fd_out[0] = read end, fd_out[1] = write end. 0 on success. */
int  pipe_new(struct file **read_end, struct file **write_end);

long pipe_read(struct pipe *p, void *buf, long n);   /* blocks; 0 = EOF */
long pipe_write(struct pipe *p, const void *buf, long n);
void pipe_close(struct pipe *p, int writer);         /* close one end */
