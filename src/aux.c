/* aux.c: Auxiliary editor commands for the ed line editor.
 *
 *  Copyright © 1993-2016 Andrew L. Moore, SlewSys Research
 *
 *  This file is part of ed.
 */

#include "ed.h"


#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif

#ifndef WIFEXITED
# define WIFEXITED(status) (((status) & 0xff) == 0)
#endif

#ifndef WEXITSTATUS
# define WEXITSTATUS(status) ((unsigned)(status) >> 8)
#endif


/* Static function declarations. */
static ed_line_node_t *append_node_to_register __P ((size_t, off_t, int,
                                               ed_buffer_t *));


#if defined (HAVE_FORK) && defined (WANT_EXTERNAL_FILTER)
# define WAITPID(pid)                                                         \
  do                                                                          \
    {                                                                         \
      waitpid (pid, &status, 0);                                              \
      if (!status && WIFEXITED (status) && WEXITSTATUS (status) == 127)       \
        {                                                                     \
          ed->exec->err = _("Child process error");                           \
          status = ERR;                                                       \
        }                                                                     \
    }                                                                         \
  while (0)


/* 
 * filter_lines: Filter a range of lines through a shell command;
 *    return status.
 */
int
filter_lines (from, to, sc, ed)
     off_t from;
     off_t to;
     const char *sc;
     ed_buffer_t *ed;
{
  FILE *ipp, *opp;
  ed_line_node_t *lp = get_line_node (from, ed);
  off_t n = from ? to - from + 1 : 0;
  pid_t shell_pid, write_pid;
  off_t size = 0;
  int ip[2], op[2];
  int status = 0;

  /* Flush any buffered I/O. */
  fflush (NULL);

  /* 
   * Create two pipes: one for writing to the shell command and
   * another for reading from it.
   */
  if (pipe (ip) < 0 || pipe (op) < 0)
    {
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Pipe open error");
      return ERR;
    }

  /* Create first child process for shell command. */
  switch (shell_pid = fork ())
    {
    case -1:
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Fork error");
      close (ip[0]), close (ip[1]);
      close (op[0]), close (op[1]);
      status = ERR;
      goto err;
    case 0:
      /* Reset signals for shell. */
      signal (SIGINT, SIG_DFL);
      signal (SIGQUIT, SIG_DFL);

      /* 
       * Redirect read/write pipes to command's standard I/O. Close
       * write end of pipe ip, ip[1], so that EOF is seen. Execute
       * shell command.
       */
      if (dup2 (ip[0], 0) < 0 || dup2 (op[1], 1) < 0 || dup2 (op[1], 2) < 0
          || close (ip[0]) < 0 || close (ip[1]) < 0
          || close (op[0]) < 0 || close (op[1]) < 0
          || execl ("/bin/sh", "sh", "-c", sc, NULL))
        {
          fprintf (stderr, "%s\n", strerror (errno));
          _exit (127 << 8);
        }

      /* NOTREACHED */
    }

  /* Close write end of pipe op, op[1], so that EOF is seen. */
  close (op[1]);

  spl1 ();
  if ((status = delete_lines (from, to, ed)) < 0)
    {
      kill (shell_pid, SIGINT);
      spl0 ();
      goto err;
    }

  /* If filtering entire file, reset state. */
  if (ed->state->lines == 0)
    {
      ed->state->is_binary = 0;
      ed->state->is_empty = 1;
    }
  spl0 ();

  /* Create second child process for writing to shell command. */
  switch (write_pid = fork ())
    {
    case -1:
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Fork error");
      (void) close (ip[1]);
      (void) close (op[0]);
      status = ERR;
      goto err;
    case 0:

      /* 
       * Close unused pipe ends, then open and write to shell
       * process's standard input.
       */
      if (close (ip[0]) < 0 || close (op[0]) < 0
          || !(ipp = fdopen (ip[1], "w"))
          || (status = write_stream (ipp, lp, n, &size, ed)) < 0
          || fclose (ipp) < 0)
        {
          fprintf (stderr, "%s\n", strerror (errno));
          status = ERR;
        }

      /* Avoid exit (3). */
      _exit (status < 0 ? 127 << 8 : 0);
    }

  /* Close unused pipe ends, then open shell process's standard output. */
  if (close (ip[0]) < 0 || close (ip[1]) < 0 || !(opp = fdopen (op[0], "r")))
    {
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Pipe open error");
      status = ERR;
      goto err;
    }

  /* Read standard output of shell process via reentrant read_stream. */
  if ((status = read_stream_r (opp, ed->state->dot, &size, ed)) < 0)
    goto err;

  printf (ed->exec->opt & SCRIPTED ? "" : "%" OFF_T_FORMAT_STRING "\n", size);

  if (fclose (opp) < 0)
    {
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Pipe close error");
      status = ERR;
      goto err;
    }
  WAITPID (write_pid);

 err:
  WAITPID (shell_pid);
  return status;
}
#endif  /* defined (HAVE_FORK) && defined (WANT_EXTERNAL_FILTER) */


/* 
 * append_node_to_register: Append node to end of given register.
 *   Return node pointer.
 */
static ed_line_node_t *
append_node_to_register (len, offset, idx, ed)
     size_t len;
     off_t offset;
     int idx;
     ed_buffer_t *ed;
{
  ed_line_node_t *lp;
  ed_line_node_t *tq = ed->core->reg->lp[idx]->q_back;

  spl1 ();
  if (!(lp = (ed_line_node_t *) malloc (ED_LINE_NODE_T_SIZE)))
    {
      fprintf (stderr, "%s\n", strerror (errno));
      ed->exec->err = _("Memory exhausted");
      spl0 ();
      return NULL;
    }
  lp->len = len;
  lp->seek = offset;

  /* APPEND_NODE is macro, so tq variable is mandatory! */
  APPEND_NODE (lp, tq);
  spl0 ();
  return lp;
}


/* 
 * append_from_register: Append lines from register to buffer after given
 *   address.
 */
int
read_from_register (addr, ed)
     off_t addr;                /* destination address */
     ed_buffer_t *ed;
{
  int read_idx = ed->core->reg->read_idx;
  ed_line_node_t *read_head = ed->core->reg->lp[read_idx];
  ed_line_node_t *lp, *rp;
  ed_undo_node_t *up = NULL;

  if (read_head)
    for (rp = read_head->q_forw; rp != read_head; rp = rp->q_forw)
      {
        spl1 ();
        if (!(lp = append_line_node (rp->len, rp->seek, addr, ed)))
          {
            spl0 ();
            return ERR;
          }
        ed->state->dot = ++addr;
        APPEND_UNDO_NODE (lp, up, addr, ed);
        ed->state->is_modified = 1;
        spl0 ();
      }
  return 0;
}


/* 
 * register_copy: Write lines from one register to another. If
 *   `append' is zero, any previous contents of target register are
 *   lost.
 */
int
register_copy (append, ed)
     int append;
     ed_buffer_t *ed;
{
  int read_idx = ed->core->reg->read_idx;
  int write_idx = ed->core->reg->write_idx;
  ed_line_node_t *read_head = ed->core->reg->lp[read_idx];
  ed_line_node_t *write_head = ed->core->reg->lp[write_idx];
  ed_line_node_t *rp;

  if (!write_head)
    {
      if (!init_register_queue (write_idx, ed))
        write_head = ed->core->reg->lp[write_idx];
      else
        return ERR;
    }
  
  /* Appending register to itself. */
  if (append && read_idx == write_idx)
    {
      ed->exec->err = _("Append register to itself not allowed.");
      return ERR;
    }

  /* Not overwriting register with itself. */
  else if (append || read_idx != write_idx)
    {
      if (!append && reset_register_queue (write_idx, ed) < 0)
        return ERR;

      if (read_head)
        for (rp = read_head->q_forw; rp != read_head; rp = rp->q_forw)
          if (!append_node_to_register (rp->len, rp->seek, write_idx, ed))
            return ERR;
    }
  return 0;
}


/* 
 * move_register: Move lines from one register to another. If `append'
 *   is zero, any previous contents of target register are lost.
 */
int
register_move (append, ed)
     int append;
     ed_buffer_t *ed;
{
  int read_idx = ed->core->reg->read_idx;
  int write_idx = ed->core->reg->write_idx;
  ed_line_node_t *read_head = ed->core->reg->lp[read_idx];
  ed_line_node_t *write_head = ed->core->reg->lp[write_idx];

  if (!write_head)
    {
      if (!init_register_queue (write_idx, ed))
        write_head = ed->core->reg->lp[write_idx];
      else
        return ERR;
    }
  
  /* Appending register to itself. */
  if (append && read_idx == write_idx)
    {
      ed->exec->err = _("Append register to itself not allowed.");
      return ERR;
    }

  /* Not overwriting register with itself. */
  else if (append || read_idx != write_idx)
    {

      if (!append && reset_register_queue (write_idx, ed) < 0)
        return ERR;

      /* Read register not empty. */
      if (read_head && read_head != read_head->q_forw)
        {
          spl1 ();
          LINK_NODES (write_head->q_back, read_head->q_forw);
          LINK_NODES (read_head->q_back, write_head);
          LINK_NODES (read_head, read_head);
          spl0 ();
        }
    }
  return 0;
}


/* reset_register_queue: Release nodes of given register. */
int
reset_register_queue (idx, ed)
     int idx;
     ed_buffer_t *ed;
{
  ed_line_node_t *head = ed->core->reg->lp[idx];
  ed_line_node_t *rp, *rn;

  if (!head)
    {
      if (!init_register_queue (idx, ed))
        head = ed->core->reg->lp[idx];
      else
        return ERR;
    }
  
  spl1 ();
  for (rp = head->q_forw; rp != head; rp = rn)
    {
      rn = rp->q_forw;
      UNLINK_NODE (rp);
      free (rp);
    }
  spl0 ();
  return 0;
}


/* 
 * write_to_register: Write addressed lines to given register. If
 *   `append' is zero, any previous register contents are lost.
 */
int
write_to_register (from, to, append, ed)
     off_t from;                /* from address */
     off_t to;                  /* to address */
     int append;
     ed_buffer_t *ed;
{
  int write_idx = ed->core->reg->write_idx;
  ed_line_node_t *write_head = ed->core->reg->lp[write_idx];
  ed_line_node_t *lp = get_line_node (from, ed);
  off_t n = from ? to - from + 1 : 0;
  
  if (!write_head)
    {
      if (!init_register_queue (write_idx, ed))
        write_head = ed->core->reg->lp[write_idx];
      else
        return ERR;
    }
  
  if (!append && reset_register_queue (write_idx, ed) < 0)
    return ERR;

  for (; n; --n, lp = lp->q_forw)
    if (!append_node_to_register (lp->len, lp->seek, write_idx, ed))
      return ERR;
  return 0;
}
