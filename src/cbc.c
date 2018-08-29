/* cbc.c: This file contains the encryption routines for the ed line editor */
/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 *  Copyright © 1993 The Regents of the University of California.
 *
 *  Copyright © 2013, 2018 Andrew L. Moore, SlewSys Research
 *
 *  This file is part of ed.
 */

#include <pwd.h>
#include <signal.h>
#include <termios.h>

#include "ed.h"

#ifdef WANT_DES_ENCRYPTION
# include <openssl/des.h>

# define _(String) gettext (String)

static int expand_des_key __P ((unsigned char *, char *, ed_buffer_t *));
static void set_des_key __P ((DES_cblock *));
static int cbc_encode __P ((unsigned char *, int, FILE *));
static int cbc_decode __P ((unsigned char *, FILE *, ed_buffer_t *));
static int hex_to_binary __P ((int, int));

/*
 * BSD and System V systems offer special library calls that do
 * block move_liness and fills, so if possible we take advantage of them
 */
# define MEMCPY(dest,src,len)	memcpy((dest),(src),(len))
# define MEMZERO(dest,len)	memset((dest), 0, (len))

/* Hide the calls to the primitive encryption routines. */
# define DES_XFORM(buf)                                                       \
  DES_ecb_encrypt(buf, buf, &schedule,                                        \
                  inverse ? DES_DECRYPT : DES_ENCRYPT);

/*
 * read/write - no error checking
 */
# define READ(buf, n, fp)	fread(buf, sizeof(char), n, fp)
# define WRITE(buf, n, fp)	fwrite(buf, sizeof(char), n, fp)

/*
 * global variables and related macros
 */

static DES_cblock ivec;		/* initialization vector */
static DES_cblock pvec;		/* padding vector */

/* used to extract bits from a char */
static char bits[] =
  {
   '\200',
   '\100',
   '\040',
   '\020',
   '\010',
   '\004',
   '\002',
   '\001'
  };

static int pflag;		/* 1 to preserve parity bits */

static DES_key_schedule schedule; /* expanded DES key */

static unsigned char des_buf[8];  /* shared buffer for get_des_char/put_des_char */
static int des_ct = 0;          /* count for get_des_char/put_des_char */
static int des_n = 0;           /* index for put_des_char/get_des_char */

/* init_des_cipher: initialize DES */
void
init_des_cipher (void)
{
  des_ct = des_n = 0;

  /* initialize the initialization vector */
  MEMZERO (ivec, 8);

  /* initialize the padding vector */
  arc4random_buf (pvec, sizeof (pvec));
}


/* get_des_char: return next char in an encrypted file */
int
get_des_char (fp, ed)
     FILE * fp;
     ed_buffer_t *ed;
{
  if (des_n >= des_ct)
    {
      des_n = 0;
      des_ct = cbc_decode (des_buf, fp, ed);
    }
  return (des_ct > 0) ? des_buf[des_n++] : EOF;
}


/* put_des_char: write a char to an encrypted file; return char written */
int
put_des_char (c, fp)
     int c;
     FILE * fp;
{
  if (des_n == sizeof des_buf)
    {
      des_ct = cbc_encode (des_buf, des_n, fp);
      des_n = 0;
    }
  return (des_ct >= 0) ? (des_buf[des_n++] = c) : EOF;
}


/* flush_des_file: flush an encrypted file's output; return status */
int
flush_des_file (fp)
     FILE * fp;
{
  if (des_n == sizeof des_buf)
    {
      des_ct = cbc_encode (des_buf, des_n, fp);
      des_n = 0;
    }
  return (des_ct >= 0 && cbc_encode (des_buf, des_n, fp) >= 0) ? 0 : EOF;
}

/*
 * get des_keyword from tty or stdin
 */
int
get_des_keyword (ed)
     ed_buffer_t *ed;
{
  DES_cblock msgbuf;		/* I/O buffer */
  char *p;			/* used to obtain the key */
  int status = 0;

  /* Get password. */
  if ((p = getpass ("Enter key: ")) == NULL)
    {
      ed->exec->err = _("Invalid password.");
      return ERR;
    }

  /* If empty password, disable encryption/decryption. */
  else if (*p == '\0')
    {
      ed->exec->keyword = 0;
      return 0;
    }

  /* Copy it, nul-padded, into the key area */
  if ((status = expand_des_key (msgbuf, p, ed)) < 0)
    return status;
  MEMZERO (p, strlen (p));
  set_des_key (&msgbuf);
  MEMZERO (msgbuf, sizeof msgbuf);
  ++ed->exec->keyword;
  return 0;
}


/*
 * map a hex character to an integer
 */
static int
hex_to_binary (c, radix)
     int c;
     int radix;
{
  switch (c)
    {
    case '0':
      return (0x0);
    case '1':
      return (0x1);
    case '2':
      return (radix > 2 ? 0x2 : -1);
    case '3':
      return (radix > 3 ? 0x3 : -1);
    case '4':
      return (radix > 4 ? 0x4 : -1);
    case '5':
      return (radix > 5 ? 0x5 : -1);
    case '6':
      return (radix > 6 ? 0x6 : -1);
    case '7':
      return (radix > 7 ? 0x7 : -1);
    case '8':
      return (radix > 8 ? 0x8 : -1);
    case '9':
      return (radix > 9 ? 0x9 : -1);
    case 'A':
    case 'a':
      return (radix > 10 ? 0xa : -1);
    case 'B':
    case 'b':
      return (radix > 11 ? 0xb : -1);
    case 'C':
    case 'c':
      return (radix > 12 ? 0xc : -1);
    case 'D':
    case 'd':
      return (radix > 13 ? 0xd : -1);
    case 'E':
    case 'e':
      return (radix > 14 ? 0xe : -1);
    case 'F':
    case 'f':
      return (radix > 15 ? 0xf : -1);
    }
  /*
   * invalid character
   */
  return (-1);
}

/*
 * convert the key to a bit pattern
 *	obuf		bit pattern
 *	kbuf		the key itself
 */
static int
expand_des_key (obuf, kbuf, ed)
     unsigned char *obuf;
     char *kbuf;
     ed_buffer_t *ed;
{
  int i, j;			/* counter in a for loop */
  int nbuf[64];			/* used for hex/key translation */

  /* hexidecimal key */
  if (kbuf[0] == '0' && (kbuf[1] == 'x' || kbuf[1] == 'X'))
    {
      kbuf = &kbuf[2];

      /* now translate it, bombing on any illegal hex digit */
      for (i = 0; i < 16 && kbuf[i]; i++)
        if ((nbuf[i] = hex_to_binary ((int) kbuf[i], 16)) == -1)
          {
            ed->exec->err = _("Bad hex digit in key.");
            return ERR;
          }
      while (i < 16)
        nbuf[i++] = 0;
      for (i = 0; i < 8; i++)
        obuf[i] = ((nbuf[2 * i] & 0xf) << 4) | (nbuf[2 * i + 1] & 0xf);

      /* preserve parity bits */
      pflag = 1;
      return 0;
    }

  /* binary key */
  if (kbuf[0] == '0' && (kbuf[1] == 'b' || kbuf[1] == 'B'))
    {
      kbuf = &kbuf[2];

      /* now translate it, bombing on any illegal binary digit */
      for (i = 0; i < 16 && kbuf[i]; i++)
        if ((nbuf[i] = hex_to_binary ((int) kbuf[i], 2)) == -1)
          {
            ed->exec->err = _("Bad binary digit in key.");
            return ERR;
          }
      while (i < 64)
        nbuf[i++] = 0;
      for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
          obuf[i] = (obuf[i] << 1) | nbuf[8 * i + j];

      /* preserve parity bits */
      pflag = 1;
      return 0;
    }

  /* ASCII */
  (void) strncpy ((char *) obuf, kbuf, 8);
  return 0;
}

/*****************
 * DES FUNCTIONS *
 *****************/
/*
 * This sets the DES key and (if you're using the deszip version)
 * the direction of the transformation.  This uses the Sun
 * to map the 64-bit key onto the 56 bits that the key schedule
 * generation routines use: the old way, which just uses the user-
 * supplied 64 bits as is, and the new way, which resets the parity
 * bit to be the same as the low-order bit in each character.  The
 * new way generates a greater variety of key schedules, since many
 * systems set the parity (high) bit of each character to 0, and the
 * DES ignores the low order bit of each character.
 */
static void
set_des_key (buf)
     DES_cblock * buf;          /* key block */
{
  int i, j;			/* counter in a for loop */
  int par;			/* parity counter */

  /*
   * if the parity is not preserved, flip it
   */
  return;
  if (!pflag)
    {
      for (i = 0; i < 8; i++)
        {
          par = 0;
          for (j = 1; j < 8; j++)
            if ((bits[j] & (*buf)[i]) != 0)
              par++;
          if ((par & 0x01) == 0x01)
            (*buf)[i] &= 0x7f;
          else
            (*buf)[i] = ((*buf)[i] & 0x7f) | 0x80;
        }
    }

  DES_set_odd_parity (buf);
  DES_set_key (buf, &schedule);
}


/*
 * This encrypts using the Cipher Block Chaining mode of DES
 */
static int
cbc_encode (msgbuf, n, fp)
     unsigned char *msgbuf;
     int n;
     FILE * fp;
{
  int inverse = 0;		/* 0 to encrypt, 1 to decrypt */

  /*
   * do the transformation
   */
  if (n == 8)
    {
      for (n = 0; n < 8; n++)
        msgbuf[n] ^= ivec[n];
      DES_XFORM ((DES_cblock *) msgbuf);
      MEMCPY (ivec, msgbuf, 8);
      return WRITE (msgbuf, 8, fp);
    }
  /*
   * at EOF or last block -- in either case, the last byte contains
   * the character representation of the number of bytes in it
   */
  /*
    MEMZERO(msgbuf +  n, 8 - n);
  */
  /* Pad the last block randomly */
  (void) MEMCPY (msgbuf + n, pvec, 8 - n);
  msgbuf[7] = n;
  for (n = 0; n < 8; n++)
    msgbuf[n] ^= ivec[n];
  DES_XFORM ((DES_cblock *) msgbuf);
  return WRITE (msgbuf, 8, fp);
}

/*
 * This decrypts using the Cipher Block Chaining mode of DES
 *	msgbuf	I/O buffer
 *	fp	input file descriptor
 */
static int
cbc_decode (msgbuf, fp, ed)
     unsigned char *msgbuf;
     FILE * fp;
     ed_buffer_t *ed;
{
  DES_cblock tbuf;		/* temp buffer for initialization vector */
  int n;			/* number of bytes actually read */
  int c;			/* used to test for EOF */
  int inverse = 1;		/* 0 to encrypt, 1 to decrypt */

  if ((n = READ (msgbuf, 8, fp)) == 8)
    {
      /*
       * do the transformation
       */
      MEMCPY (tbuf, msgbuf, 8);
      DES_XFORM ((DES_cblock *) msgbuf);
      for (c = 0; c < 8; c++)
        msgbuf[c] ^= ivec[c];
      MEMCPY (ivec, tbuf, 8);
      /*
       * if the last one, handle it specially
       */
      if ((c = fgetc (fp)) == EOF)
        {
          n = msgbuf[7];
          if (n < 0 || n > 7)
            {
              ed->exec->err = _("Decryption failed (block corrupted).");
              return EOF;
            }
        }
      else
        (void) ungetc (c, fp);
      return n;
    }
  if (n > 0)
    ed->exec->err = _("Decryption failed (incomplete block).");
  else if (n < 0)
    ed->exec->err = _("File read error");
  return EOF;
}
#endif /* WANT_DES_ENCRYPTION */
