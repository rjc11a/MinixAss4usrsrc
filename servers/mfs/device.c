
#include "fs.h"
#include <fcntl.h>
#include <assert.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/endpoint.h>
#include <minix/ioctl.h>
#include <minix/safecopies.h>
#include <minix/u64.h>
#include <string.h>
#include "inode.h"
#include "super.h"
#include "const.h"
#include "drivers.h"

#include <minix/vfsif.h>

PRIVATE int dummyproc;

FORWARD _PROTOTYPE( int safe_io_conversion, (endpoint_t,
  cp_grant_id_t *, int *, cp_grant_id_t *, int, endpoint_t *,
  void **, int *, vir_bytes));
FORWARD _PROTOTYPE( void safe_io_cleanup, (cp_grant_id_t, cp_grant_id_t *,
	int));

/*===========================================================================*
 *				fs_clone_opcl   			     *
 *===========================================================================*/
PUBLIC int fs_clone_opcl(void)
{
 /* A new minor device number has been returned.
  * Create a temporary device file to hold it. 
  */
  struct inode *ip;
  dev_t dev; 

  dev = fs_m_in.REQ_DEV;   /* Device number */

  ip = alloc_inode(fs_dev, ALL_MODES | I_CHAR_SPECIAL);

  if (ip == NIL_INODE) return err_code;

  ip->i_zone[0] = dev;

  fs_m_out.m_source = ip->i_dev;
  fs_m_out.RES_INODE_NR = ip->i_num;
  fs_m_out.RES_MODE = ip->i_mode;
  return OK;
}


/*===========================================================================*
 *				fs_new_driver   			     *
 *===========================================================================*/
PUBLIC int fs_new_driver(void)
{
 /* New driver endpoint for this device */
  driver_endpoints[(fs_m_in.REQ_DEV >> MAJOR) & BYTE].driver_e =
      fs_m_in.REQ_DRIVER_E;
  return OK;
}


/*===========================================================================*
 *				safe_io_conversion			     *
 *===========================================================================*/
PRIVATE int safe_io_conversion(driver, gid, op, gids, gids_size,
	io_ept, buf, vec_grants, bytes)
endpoint_t driver;
cp_grant_id_t *gid;
int *op;
cp_grant_id_t *gids;
int gids_size;
endpoint_t *io_ept;
void **buf;
int *vec_grants;
vir_bytes bytes;
{
	int access = 0, size;
	int j;
	iovec_t *v;
	static iovec_t new_iovec[NR_IOREQS];

	/* Number of grants allocated in vector I/O. */
	*vec_grants = 0;

	/* Driver can handle it - change request to a safe one. */

	*gid = GRANT_INVALID;

	switch(*op) {
		case MFS_DEV_READ:
		case MFS_DEV_WRITE:
			/* Change to safe op. */
			*op = *op == MFS_DEV_READ ? DEV_READ_S : DEV_WRITE_S;

			if((*gid=cpf_grant_direct(driver, (vir_bytes) *buf, 
				bytes, *op == DEV_READ_S ? CPF_WRITE : 
				CPF_READ)) < 0) {
				panic(__FILE__,
				 "cpf_grant_magic of buffer failed\n", NO_NUM);
			}

			break;
		case MFS_DEV_GATHER:
		case MFS_DEV_SCATTER:
			/* Change to safe op. */
			*op = *op == MFS_DEV_GATHER ?
				DEV_GATHER_S : DEV_SCATTER_S;

			/* Grant access to my new i/o vector. */
			if((*gid = cpf_grant_direct(driver,
			  (vir_bytes) new_iovec, bytes * sizeof(iovec_t),
			  CPF_READ | CPF_WRITE)) < 0) {
				panic(__FILE__,
				"cpf_grant_direct of vector failed", NO_NUM);
			}
			v = (iovec_t *) *buf;
			/* Grant access to i/o buffers. */
			for(j = 0; j < bytes; j++) {
			   if(j >= NR_IOREQS) 
				panic(__FILE__, "vec too big", bytes);
			   new_iovec[j].iov_addr = gids[j] =
			     cpf_grant_direct(driver, (vir_bytes)
			     v[j].iov_addr, v[j].iov_size,
			     *op == DEV_GATHER_S ? CPF_WRITE : CPF_READ);
			   if(!GRANT_VALID(gids[j])) {
				panic(__FILE__, "mfs: grant to iovec buf failed",
				 NO_NUM);
			   }
			   new_iovec[j].iov_size = v[j].iov_size;
			   (*vec_grants)++;
			}

			/* Set user's vector to the new one. */
			*buf = new_iovec;
			break;
	}

	/* If we have converted to a safe operation, I/O
	 * endpoint becomes FS if it wasn't already.
	 */
	if(GRANT_VALID(*gid)) {
		*io_ept = SELF_E;
		return 1;
	}

	/* Not converted to a safe operation (because there is no
	 * copying involved in this operation).
	 */
	return 0;
}

/*===========================================================================*
 *			safe_io_cleanup					     *
 *===========================================================================*/
PRIVATE void safe_io_cleanup(gid, gids, gids_size)
cp_grant_id_t gid;
cp_grant_id_t *gids;
int gids_size;
{
/* Free resources (specifically, grants) allocated by safe_io_conversion(). */
	int j;

  	cpf_revoke(gid);

	for(j = 0; j < gids_size; j++)
		cpf_revoke(gids[j]);

	return;
}

/*===========================================================================*
 *			block_dev_io					     *
 *===========================================================================*/
PUBLIC int block_dev_io(op, dev, proc_e, buf, pos, bytes, flags)
int op;				/* MFS_DEV_READ, MFS_DEV_WRITE, etc. */
dev_t dev;			/* major-minor device number */
int proc_e;			/* in whose address space is buf? */
void *buf;			/* virtual address of the buffer */
u64_t pos;			/* byte position */
int bytes;			/* how many bytes to transfer */
int flags;			/* special flags, like O_NONBLOCK */
{
/* Read or write from a device.  The parameter 'dev' tells which one. */
  struct dmap *dp;
  int r, safe;
  message m;
  iovec_t *v;
  cp_grant_id_t gid = GRANT_INVALID;
  int vec_grants;
  int op_used;
  void *buf_used;
  static cp_grant_id_t gids[NR_IOREQS];
  endpoint_t driver_e;

  /* Determine driver endpoint for this device */
  driver_e = driver_endpoints[(dev >> MAJOR) & BYTE].driver_e;
  
  /* See if driver is roughly valid. */
  if (driver_e == NONE) {
      printf("MFS(%d) block_dev_io: no driver for dev %x\n", SELF_E, dev);
      return EDSTDIED;
  }
  
  /* The io vector copying relies on this I/O being for FS itself. */
  if(proc_e != SELF_E) {
      printf("MFS(%d) doing block_dev_io for non-self %d\n", SELF_E, proc_e);
      panic(__FILE__, "doing block_dev_io for non-self", proc_e);
  }
  
  /* By default, these are right. */
  m.IO_ENDPT = proc_e;
  m.ADDRESS  = buf;
  buf_used = buf;

  /* Convert parameters to 'safe mode'. */
  op_used = op;
  safe = safe_io_conversion(driver_e, &gid,
          &op_used, gids, NR_IOREQS, &m.IO_ENDPT, &buf_used,
          &vec_grants, bytes);

  /* Set up rest of the message. */
  if (safe) m.IO_GRANT = (char *) gid;

  m.m_type   = op_used;
  m.DEVICE   = (dev >> MINOR) & BYTE;
  m.POSITION = ex64lo(pos);
  m.COUNT    = bytes;
  m.HIGHPOS  = ex64hi(pos);

  /* Call the task. */
  r = sendrec(driver_e, &m);

  /* As block I/O never SUSPENDs, safe cleanup must be done whether
   * the I/O succeeded or not. */
  if (safe) safe_io_cleanup(gid, gids, vec_grants);
  
  /* RECOVERY:
   * - send back dead driver number
   * - VFS unmaps it, waits for new driver
   * - VFS sends the new driver endp for the FS proc and the request again 
   */
  if (r != OK) {
      if (r == EDEADSRCDST || r == EDSTDIED || r == ESRCDIED) {
          printf("MFS(%d) dead driver %d\n", SELF_E, driver_e);
          driver_endpoints[(dev >> MAJOR) & BYTE].driver_e = NONE;
          return r;
          /*dmap_unmap_by_endpt(task_nr);    <- in the VFS proc...  */
      }
      else if (r == ELOCKED) {
          printf("MFS(%d) ELOCKED talking to %d\n", SELF_E, driver_e);
          return r;
      }
      else 
          panic(__FILE__,"call_task: can't send/receive", r);
  } 
  else {
      /* Did the process we did the sendrec() for get a result? */
      if (m.REP_ENDPT != proc_e) {
          printf("MFS(%d) strange device reply from %d, type = %d, proc = %d (not %d) (2) ignored\n", SELF_E, m.m_source, m.m_type, proc_e, m.REP_ENDPT);
          r = EIO;
      }
  }

  /* Task has completed.  See if call completed. */
  if (m.REP_STATUS == SUSPEND) {
      panic(__FILE__, "MFS block_dev_io: driver returned SUSPEND", NO_NUM);
  }

  if(buf != buf_used && r == OK) {
      memcpy(buf, buf_used, bytes * sizeof(iovec_t));
  }

  return(m.REP_STATUS);
}








