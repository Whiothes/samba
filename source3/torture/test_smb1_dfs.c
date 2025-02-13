/*
   Unix SMB/CIFS implementation.
   SMB1 DFS tests.
   Copyright (C) Jeremy Allison 2022.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "torture/proto.h"
#include "client.h"
#include "trans2.h"
#include "../libcli/smb/smbXcli_base.h"
#include "libcli/security/security.h"
#include "libsmb/proto.h"
#include "auth/credentials/credentials.h"
#include "auth/gensec/gensec.h"
#include "auth_generic.h"
#include "../librpc/ndr/libndr.h"
#include "libsmb/clirap.h"
#include "async_smb.h"
#include "../lib/util/tevent_ntstatus.h"
#include "lib/util/time_basic.h"

extern fstring host, workgroup, share, password, username, myname;
extern struct cli_credentials *torture_creds;

/*
 * Open an SMB1 file readonly and return the create time.
 */
static NTSTATUS get_smb1_crtime(struct cli_state *cli,
				const char *pathname,
				struct timespec *pcrtime)
{
	NTSTATUS status;
	uint16_t fnum = 0;
	struct timespec crtime = {0};

	/*
	 * Open the file.
	 */

	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   pathname,
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_FILE_READ_DATA|
					SEC_FILE_READ_ATTRIBUTE, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_OPEN, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/*
	 * Get the create time. Note - we can use
	 * a higher-level cli_XXX function here
	 * for SMB1 as cli_qfileinfo_basic()
	 * doesn't use any pathnames, only fnums
	 * so it isn't affected by DFS pathnames.
	 */
	status = cli_qfileinfo_basic(cli,
				     fnum,
				     NULL, /* attr */
				     NULL, /* size */
				     &crtime, /* create_time */
				     NULL, /* access_time */
				     NULL, /* write_time */
				     NULL, /* change_time */
				     NULL);
	if (NT_STATUS_IS_OK(status)) {
		*pcrtime = crtime;
	}

	(void)smb1cli_close(cli->conn,
			    cli->timeout,
			    cli->smb1.pid,
			    cli->smb1.tcon,
			    cli->smb1.session,
			    fnum,
			    0); /* last_modified */
	return status;
}

/*
 * Check a crtime matches a given SMB1 path.
 */
static bool smb1_crtime_matches(struct cli_state *cli,
				const char *match_pathname,
				struct timespec crtime_tomatch,
				const char *test_pathname)
{
	struct timespec test_crtime = { 0 };
	NTSTATUS status;
	bool equal = false;

	status = get_smb1_crtime(cli,
				test_pathname,
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s: Failed to get crtime "
			"for %s, (%s)\n",
			__func__,
			test_pathname,
			nt_errstr(status));
		return false;
	}
	equal = (timespec_compare(&test_crtime, &crtime_tomatch) == 0);
	if (!equal) {
		struct timeval_buf test_buf;
		struct timeval_buf tomatch_buf;
		printf("%s: crtime missmatch "
			"%s:crtime_tomatch=%s, %s:test_crtime = %s\n",
			__func__,
			match_pathname,
			timespec_string_buf(&crtime_tomatch,
				true,
				&tomatch_buf),
			test_pathname,
			timespec_string_buf(&test_crtime,
				true,
				&test_buf));
		return false;
	}
	return true;
}

/*
 * Delete an SMB1 file on a DFS share.
 */
static NTSTATUS smb1_dfs_delete(struct cli_state *cli,
				const char *pathname)
{
	NTSTATUS status;
	uint16_t fnum = 0;

	/*
	 * Open the file.
	 */

	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   pathname,
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_STD_DELETE, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_OPEN, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/*
	 * Set delete on close. Note - we can use
	 * a higher-level cli_XXX function here
	 * for SMB1 as cli_nt_delete_on_close()
	 * doesn't use any pathnames, only fnums
	 * so it isn't affected by DFS pathnames.
	 */
	/*
	 */
	status = cli_nt_delete_on_close(cli, fnum, 1);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	return smb1cli_close(cli->conn,
			    cli->timeout,
			    cli->smb1.pid,
			    cli->smb1.tcon,
			    cli->smb1.session,
			    fnum,
			    0); /* last_modified */
}

static void smb1_mv_done(struct tevent_req *subreq);

struct smb1_mv_state {
	uint16_t vwv[1];
};

static struct tevent_req *smb1_mv_send(TALLOC_CTX *mem_ctx,
				       struct tevent_context *ev,
				       struct cli_state *cli,
				       const char *src_dfs_name,
				       const char *target_name)
{
	uint8_t *bytes = NULL;
	struct tevent_req *req = NULL;
	struct tevent_req *subreq = NULL;
	struct smb1_mv_state *state = NULL;

	req = tevent_req_create(mem_ctx,
				&state,
				struct smb1_mv_state);
        if (req == NULL) {
                return NULL;
        }

	PUSH_LE_U16(state->vwv,
		    0,
		    FILE_ATTRIBUTE_SYSTEM |
		    FILE_ATTRIBUTE_HIDDEN |
		    FILE_ATTRIBUTE_DIRECTORY);

	bytes = talloc_array(state, uint8_t, 1);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}
	bytes[0] = 4;
	bytes = smb_bytes_push_str(bytes,
				   smbXcli_conn_use_unicode(cli->conn),
				   src_dfs_name,
				   strlen(src_dfs_name)+1,
				   NULL);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	bytes = talloc_realloc(state,
			       bytes,
			       uint8_t,
			       talloc_get_size(bytes)+1);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	bytes[talloc_get_size(bytes)-1] = 4;
	bytes = smb_bytes_push_str(bytes,
				   smbXcli_conn_use_unicode(cli->conn),
				   target_name,
                                   strlen(target_name)+1,
				   NULL);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	subreq = cli_smb_send(state,
			      ev,
			      cli,
			      SMBmv,
			      0, /* additional_flags */
			      0, /* additional_flags2 */
			      1,
			      state->vwv,
			      talloc_get_size(bytes),
			      bytes);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, smb1_mv_done, req);
	return req;
}

static void smb1_mv_done(struct tevent_req *subreq)
{
	NTSTATUS status = cli_smb_recv(subreq,
				       NULL,
				       NULL,
				       0,
				       NULL,
				       NULL,
				       NULL,
				       NULL);
	tevent_req_simple_finish_ntstatus(subreq,
					  status);
}

static NTSTATUS smb1_mv_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}

/*
 * Rename an SMB1 file on a DFS share. SMBmv version.
 */
static NTSTATUS smb1_mv(struct cli_state *cli,
			const char *src_dfs_name,
			const char *target_name)
{
	TALLOC_CTX *frame = NULL;
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status;

	frame = talloc_stackframe();

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
                status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	req = smb1_mv_send(frame,
			   ev,
			   cli,
			   src_dfs_name,
			   target_name);
	if (req == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}

	status = smb1_mv_recv(req);

  fail:

	TALLOC_FREE(frame);
	return status;
}

static bool test_smb1_mv(struct cli_state *cli,
			 const char *src_dfs_name)
{
	struct timespec test_timespec = { 0 };
	NTSTATUS status;

	status = smb1_mv(cli,
			 src_dfs_name,
			 "BAD\\BAD\\renamed_file");
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMBmv of %s -> %s should succeed "
			"got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\renamed_file",
			nt_errstr(status));
		return false;
	}

	/* Ensure we did rename. */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\renamed_file",
				&test_timespec);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        "BAD\\BAD\\renamed_file",
                        nt_errstr(status));
                return false;
        }

	/* Put it back. */
	status = smb1_mv(cli,
			 "BAD\\BAD\\renamed_file",
			 src_dfs_name);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMBmv of %s -> %s should succeed "
			"got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\renamed_file",
			src_dfs_name,
			nt_errstr(status));
		return false;
	}

	/* Ensure we did put it back. */
	status = get_smb1_crtime(cli,
				src_dfs_name,
				&test_timespec);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        src_dfs_name,
                        nt_errstr(status));
                return false;
        }

	/* Try with a non-DFS name. */
	status = smb1_mv(cli,
			 src_dfs_name,
			 "renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD)) {
		/* Fails I think as target becomes "" on server. */
		printf("%s:%d SMBmv of %s -> %s should get "
			"NT_STATUS_OBJECT_PATH_SYNTAX_BAD got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"renamed_file",
			nt_errstr(status));
		return false;
	}

	/* Try with a non-DFS name. */
	status = smb1_mv(cli,
			 src_dfs_name,
			 "BAD\\renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD)) {
		/* Fails I think as target becomes "" on server. */
		printf("%s:%d SMBmv of %s -> %s should get "
			"NT_STATUS_OBJECT_PATH_SYNTAX_BAD got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\renamed_file",
			nt_errstr(status));
		return false;
	}
	return true;
}

static void smb1_setpathinfo_done(struct tevent_req *subreq);

struct smb1_setpathinfo_state {
	uint16_t setup;
	uint8_t *param;
	uint8_t *data;
};

static struct tevent_req *smb1_setpathinfo_send(TALLOC_CTX *mem_ctx,
						struct tevent_context *ev,
						struct cli_state *cli,
						const char *src_dfs_name,
						const char *target_name,
						uint16_t info_level)
{
	struct tevent_req *req = NULL;
	struct tevent_req *subreq = NULL;
	struct smb1_setpathinfo_state *state = NULL;
	smb_ucs2_t *converted_str = NULL;
	size_t converted_size_bytes = 0;
	bool ok = false;

	req = tevent_req_create(mem_ctx,
				&state,
				struct smb1_setpathinfo_state);
        if (req == NULL) {
                return NULL;
        }

	PUSH_LE_U16(&state->setup, 0, TRANSACT2_SETPATHINFO);

	state->param = talloc_zero_array(state, uint8_t, 6);
	if (tevent_req_nomem(state->param, req)) {
		return tevent_req_post(req, ev);
	}
	PUSH_LE_U16(state->param, 0, info_level);

	state->param = trans2_bytes_push_str(state->param,
					     smbXcli_conn_use_unicode(cli->conn),
					     src_dfs_name,
					     strlen(src_dfs_name)+1,
					     NULL);
	if (tevent_req_nomem(state->param, req)) {
		return tevent_req_post(req, ev);
	}

	ok = push_ucs2_talloc(state,
			      &converted_str,
			      target_name,
			      &converted_size_bytes);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}

	/*
	 * W2K8 insists the dest name is not null
	 * terminated. Remove the last 2 zero bytes
	 * and reduce the name length.
	 */

	if (converted_size_bytes < 2) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}
	converted_size_bytes -= 2;

	state->data = talloc_zero_array(state,
					uint8_t,
					12 + converted_size_bytes);
	if (tevent_req_nomem(state->data, req)) {
		return tevent_req_post(req, ev);
	}

	SIVAL(state->data, 8, converted_size_bytes);
	memcpy(state->data + 12, converted_str, converted_size_bytes);

	subreq = cli_trans_send(state, /* mem ctx. */
				ev,/* event ctx. */
				cli,/* cli_state. */
				0,/* additional_flags2 */
				SMBtrans2,              /* cmd. */
				NULL,/* pipe name. */
				-1,/* fid. */
				0,/* function. */
				0,/* flags. */
				&state->setup,/* setup. */
				1,/* num setup uint16_t words. */
				0,/* max returned setup. */
				state->param,/* param. */
				talloc_get_size(state->param),/* num param. */
				2,/* max returned param. */
				state->data,/* data. */
				talloc_get_size(state->data),/* num data. */
				0);/* max returned data. */

	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, smb1_setpathinfo_done, req);
	return req;
}

static void smb1_setpathinfo_done(struct tevent_req *subreq)
{
	NTSTATUS status = cli_trans_recv(subreq,
					 NULL,
					 NULL,
					 NULL,
					 0,
					 NULL,
                                         NULL,
					 0,
					 NULL,
					 NULL,
					 0,
					 NULL);
	tevent_req_simple_finish_ntstatus(subreq,
					  status);
}

static NTSTATUS smb1_setpathinfo_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}

/*
 * Rename or hardlink an SMB1 file on a DFS share. SMB1 setpathinfo
 * (pathnames only) version.
 */
static NTSTATUS smb1_setpathinfo(struct cli_state *cli,
				 const char *src_dfs_name,
				 const char *target_name,
				 uint16_t info_level)
{
	TALLOC_CTX *frame = NULL;
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status;

	frame = talloc_stackframe();

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
                status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	req = smb1_setpathinfo_send(frame,
				    ev,
				    cli,
				    src_dfs_name,
				    target_name,
				    info_level);
	if (req == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}

	status = smb1_setpathinfo_recv(req);

  fail:

	TALLOC_FREE(frame);
	return status;
}

static NTSTATUS smb1_setpathinfo_rename(struct cli_state *cli,
					const char *src_dfs_name,
					const char *target_name)
{
	return smb1_setpathinfo(cli,
				src_dfs_name,
				target_name,
				SMB_FILE_RENAME_INFORMATION);
}

static bool test_smb1_setpathinfo_rename(struct cli_state *cli,
					 const char *src_dfs_name)
{
	struct timespec test_crtime = { 0 };
	NTSTATUS status;
	const char *putback_path = NULL;

	/*
	 * On Windows, setpathinfo rename where the target contains
	 * any directory separator returns STATUS_NOT_SUPPORTED.
	 *
	 * MS-SMB behavior note: <133> Section 3.3.5.10.6:
	 *
	 * "If the file name pointed to by the FileName parameter of the
	 * FILE_RENAME_INFORMATION structure contains a separator character,
	 * then the request fails with STATUS_NOT_SUPPORTED."
	 */
	status = smb1_setpathinfo_rename(cli,
					 src_dfs_name,
					 "BAD\\BAD\\renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_NOT_SUPPORTED)) {
		printf("%s:%d SMB1 setpathinfo rename of %s -> %s should get "
			"NT_STATUS_NOT_SUPPORTED got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\renamed_file",
			nt_errstr(status));
		return false;
	}

	/* Try with a non-DFS name. */
	status = smb1_setpathinfo_rename(cli,
					 src_dfs_name,
					 "renamed_file");
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMB1 setpathinfo rename of %s -> %s "
			"should succeed got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"renamed_file",
			nt_errstr(status));
		return false;
	}

	/* Ensure we did rename. */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\renamed_file",
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        "BAD\\BAD\\renamed_file",
                        nt_errstr(status));
                return false;
        }

	/*
	 * To put it back we need to reverse the DFS-ness of src
	 * and destination paths.
	 */
	putback_path = strrchr(src_dfs_name, '\\');
	if (putback_path == NULL) {
		printf("%s:%d non DFS path %s passed. Internal error\n",
			__FILE__,
			__LINE__,
			src_dfs_name);
		return false;
	}
	/* Walk past the last '\\' */
	putback_path++;

	/* Put it back. */
	status = smb1_setpathinfo_rename(cli,
					 "BAD\\BAD\\renamed_file",
					 putback_path);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMB1 setpathinfo rename of %s -> %s "
			"should succeed got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\renamed_file",
			putback_path,
			nt_errstr(status));
		return false;
	}

	/* Ensure we did rename. */
	status = get_smb1_crtime(cli,
				src_dfs_name,
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        src_dfs_name,
                        nt_errstr(status));
                return false;
        }

	return true;
}

static NTSTATUS smb1_setpathinfo_hardlink(struct cli_state *cli,
					  const char *src_dfs_name,
					  const char *target_name)
{
	return smb1_setpathinfo(cli,
				src_dfs_name,
				target_name,
				SMB_FILE_LINK_INFORMATION);
}

static bool test_smb1_setpathinfo_hardlink(struct cli_state *cli,
					   const char *src_dfs_name)
{
	NTSTATUS status;

	/*
	 * On Windows, setpathinfo rename where the target contains
	 * any directory separator returns STATUS_NOT_SUPPORTED.
	 *
	 * MS-SMB behavior note: <133> Section 3.3.5.10.6:
	 *
	 * "If the file name pointed to by the FileName parameter of the
	 * FILE_RENAME_INFORMATION structure contains a separator character,
	 * then the request fails with STATUS_NOT_SUPPORTED."
	 *
	 * setpathinfo info level SMB_FILE_LINK_INFORMATION
	 * seems to do the same, but this could be an artifact
	 * of the Windows version tested (Win2K8). I will
	 * revisit this when I'm able to test against
	 * a later Windows version with a DFS server.
	 */
	status = smb1_setpathinfo_hardlink(cli,
					 src_dfs_name,
					 "BAD\\BAD\\hlink");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_NOT_SUPPORTED)) {
		printf("%s:%d SMB1 setpathinfo hardlink of %s -> %s should get "
			"NT_STATUS_NOT_SUPPORTED got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\hlink",
			nt_errstr(status));
		return false;
	}

	/* Try with a non-DFS name. */
	/*
	 * At least on Windows 2008 this also fails with
	 * NT_STATUS_NOT_SUPPORTED, leading me to believe
	 * setting hardlinks is only supported via NTrename
	 * in SMB1.
	 */
	status = smb1_setpathinfo_hardlink(cli,
					   src_dfs_name,
					   "hlink");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_NOT_SUPPORTED)) {
		printf("%s:%d SMB1 setpathinfo hardlink of %s -> %s should get "
			"NT_STATUS_NOT_SUPPORTED got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"hlink",
			nt_errstr(status));
		return false;
	}
	return true;
}

static void smb1_ntrename_done(struct tevent_req *subreq);

struct smb1_ntrename_state {
	uint16_t vwv[4];
};

static struct tevent_req *smb1_ntrename_send(TALLOC_CTX *mem_ctx,
					     struct tevent_context *ev,
					     struct cli_state *cli,
					     const char *src_dfs_name,
					     const char *target_name,
					     uint16_t rename_flag)
{
	struct tevent_req *req = NULL;
	struct tevent_req *subreq = NULL;
	struct smb1_ntrename_state *state = NULL;
	uint8_t *bytes = NULL;

	req = tevent_req_create(mem_ctx,
				&state,
				struct smb1_ntrename_state);
        if (req == NULL) {
                return NULL;
        }

	PUSH_LE_U16(state->vwv,
		0,
		FILE_ATTRIBUTE_SYSTEM |
			FILE_ATTRIBUTE_HIDDEN |
			FILE_ATTRIBUTE_DIRECTORY);
        PUSH_LE_U16(state->vwv, 2, rename_flag);

	bytes = talloc_array(state, uint8_t, 1);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	bytes[0] = 4;
	bytes = smb_bytes_push_str(bytes,
				   smbXcli_conn_use_unicode(cli->conn),
				   src_dfs_name,
				   strlen(src_dfs_name)+1,
				   NULL);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}
	bytes = talloc_realloc(state,
			       bytes,
			       uint8_t,
			       talloc_get_size(bytes)+1);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	bytes[talloc_get_size(bytes)-1] = 4;
	bytes = smb_bytes_push_str(bytes,
				   smbXcli_conn_use_unicode(cli->conn),
				   target_name,
				   strlen(target_name)+1,
				   NULL);
	if (tevent_req_nomem(bytes, req)) {
		return tevent_req_post(req, ev);
	}

	subreq = cli_smb_send(state,
			      ev,
			      cli,
			      SMBntrename,
			      0, /* additional_flags */
			      0, /* additional_flags2 */
			      4,
			      state->vwv,
			      talloc_get_size(bytes),
			      bytes);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, smb1_ntrename_done, req);
	return req;
}

static void smb1_ntrename_done(struct tevent_req *subreq)
{
	NTSTATUS status = cli_smb_recv(subreq,
				       NULL,
				       NULL,
				       0,
				       NULL,
				       NULL,
				       NULL,
				       NULL);
	tevent_req_simple_finish_ntstatus(subreq, status);
}

static NTSTATUS smb1_ntrename_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}

/*
 * Rename or hardlink an SMB1 file on a DFS share. SMB1 ntrename version.
 * (pathnames only).
 */
static NTSTATUS smb1_ntrename(struct cli_state *cli,
			      const char *src_dfs_name,
			      const char *target_name,
			      uint16_t rename_flag)
{
	TALLOC_CTX *frame = NULL;
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status;

	frame = talloc_stackframe();

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
                status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	req = smb1_ntrename_send(frame,
				 ev,
				 cli,
				 src_dfs_name,
				 target_name,
				 rename_flag);
	if (req == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}

	status = smb1_ntrename_recv(req);

  fail:

	TALLOC_FREE(frame);
	return status;
}
/*
 * Rename an SMB1 file on a DFS share. SMB1 ntrename version.
 */
static NTSTATUS smb1_ntrename_rename(struct cli_state *cli,
				       const char *src_dfs_name,
				       const char *target_name)
{
	return smb1_ntrename(cli,
			     src_dfs_name,
			     target_name,
			     RENAME_FLAG_RENAME);
}


static bool test_smb1_ntrename_rename(struct cli_state *cli,
				      const char *src_dfs_name)
{
	struct timespec test_crtime = { 0 };
	NTSTATUS status;

	/* Try with a non-DFS name. */
	status = smb1_ntrename_rename(cli,
				      src_dfs_name,
				      "renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD)) {
		/* Fails I think as target becomes "" on server. */
		printf("%s:%d SMB1 ntrename rename of %s -> %s should get "
			"NT_STATUS_OBJECT_PATH_SYNTAX_BAD got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"renamed_file",
			nt_errstr(status));
		return false;
	}

	status = smb1_ntrename_rename(cli,
				      src_dfs_name,
				      "BAD\\BAD\\renamed_file");
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMB1 ntrename rename of %s -> %s should "
			"succeed got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\renamed_file",
			nt_errstr(status));
		return false;
	}

	/* Ensure we did rename. */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\renamed_file",
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        "BAD\\BAD\\renamed_file",
                        nt_errstr(status));
                return false;
        }

	/* Put it back. */
	status = smb1_ntrename_rename(cli,
				      "BAD\\BAD\\renamed_file",
				       src_dfs_name);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMB1 ntrename rename of %s -> %s "
			"should succeed got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\renamed_file",
			src_dfs_name,
			nt_errstr(status));
		return false;
	}

	/* Ensure we did rename. */
	status = get_smb1_crtime(cli,
				src_dfs_name,
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        src_dfs_name,
                        nt_errstr(status));
                return false;
        }

	return true;
}

/*
 * Hard link an SMB1 file on a DFS share. SMB1 ntrename version.
 */
static NTSTATUS smb1_ntrename_hardlink(struct cli_state *cli,
				       const char *src_dfs_name,
				       const char *target_name)
{
	return smb1_ntrename(cli,
			     src_dfs_name,
			     target_name,
			     RENAME_FLAG_HARD_LINK);
}

static bool test_smb1_ntrename_hardlink(struct cli_state *cli,
					const char *src_dfs_name)
{
	struct timespec test_crtime = { 0 };
	NTSTATUS status;
	bool retval = false;

	/* Try with a non-DFS name. */
	status = smb1_ntrename_hardlink(cli,
					src_dfs_name,
					"hlink");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD)) {
		/* Fails I think as target becomes "" on server. */
		printf("%s:%d SMB1 ntrename of %s -> %s should get "
			"NT_STATUS_OBJECT_PATH_SYNTAX_BAD got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"hlink",
			nt_errstr(status));
		return false;
	}

	status = smb1_ntrename_hardlink(cli,
					src_dfs_name,
					"BAD\\BAD\\hlink");
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d SMB1 ntrename hardlink of %s -> %s "
			"should succeed got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\hlink",
			nt_errstr(status));
		goto out;
	}

	/* Ensure we did hardlink. */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\hlink",
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime "
			"for %s, (%s)\n",
			__FILE__,
			__LINE__,
                        "BAD\\BAD\\hlink",
                        nt_errstr(status));
		goto out;
        }

	retval = smb1_crtime_matches(cli,
				    "BAD\\BAD\\hlink",
				    test_crtime,
				    src_dfs_name);
	if (!retval) {
		printf("%s:%d smb1_crtime_matches failed for "
			"%s %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
                        "BAD\\BAD\\hlink");
		goto out;
	}

  out:

	/* Remove the hardlink to clean up. */
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\hlink");
	return retval;
}

static void smb1_setfileinfo_done(struct tevent_req *subreq);

struct smb1_setfileinfo_state {
	uint16_t setup;
	uint8_t param[6];
	uint8_t *data;
};

static struct tevent_req *smb1_setfileinfo_send(TALLOC_CTX *mem_ctx,
						struct tevent_context *ev,
						struct cli_state *cli,
						uint16_t fnum,
						const char *target_name,
						uint16_t info_level)
{
	struct tevent_req *req = NULL;
	struct tevent_req *subreq = NULL;
	struct smb1_setfileinfo_state *state = NULL;
	smb_ucs2_t *converted_str = NULL;
	size_t converted_size_bytes = 0;
	bool ok = false;

	req = tevent_req_create(mem_ctx,
				&state,
				struct smb1_setfileinfo_state);
        if (req == NULL) {
                return NULL;
        }

	PUSH_LE_U16(&state->setup, 0, TRANSACT2_SETPATHINFO);

	PUSH_LE_U16(state->param, 0, fnum);
	PUSH_LE_U16(state->param, 2, info_level);

	ok = push_ucs2_talloc(state,
			      &converted_str,
			      target_name,
			      &converted_size_bytes);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}

	/*
	 * W2K8 insists the dest name is not null
	 * terminated. Remove the last 2 zero bytes
	 * and reduce the name length.
	 */

	if (converted_size_bytes < 2) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}
	converted_size_bytes -= 2;

	state->data = talloc_zero_array(state,
					uint8_t,
					12 + converted_size_bytes);
	if (tevent_req_nomem(state->data, req)) {
		return tevent_req_post(req, ev);
	}

	SIVAL(state->data, 8, converted_size_bytes);
	memcpy(state->data + 12, converted_str, converted_size_bytes);

	subreq = cli_trans_send(state, /* mem ctx. */
				ev,/* event ctx. */
				cli,/* cli_state. */
				0,/* additional_flags2 */
				SMBtrans2,              /* cmd. */
				NULL,/* pipe name. */
				-1,/* fid. */
				0,/* function. */
				0,/* flags. */
				&state->setup,/* setup. */
				1,/* num setup uint16_t words. */
				0,/* max returned setup. */
				state->param,/* param. */
				6,/* num param. */
				2,/* max returned param. */
				state->data,/* data. */
				talloc_get_size(state->data),/* num data. */
				0);/* max returned data. */

	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, smb1_setfileinfo_done, req);
	return req;
}

static void smb1_setfileinfo_done(struct tevent_req *subreq)
{
	NTSTATUS status = cli_trans_recv(subreq,
					 NULL,
					 NULL,
					 NULL,
					 0,
					 NULL,
                                         NULL,
					 0,
					 NULL,
					 NULL,
					 0,
					 NULL);
	tevent_req_simple_finish_ntstatus(subreq,
					  status);
}

static NTSTATUS smb1_setfileinfo_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}

/*
 * Rename or hardlink an SMB1 file on a DFS share.
 * setfileinfo (file handle + target pathname) version.
 */
static NTSTATUS smb1_setfileinfo(struct cli_state *cli,
				 uint16_t fnum,
				 const char *target_name,
				 uint16_t info_level)
{
	TALLOC_CTX *frame = NULL;
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status;

	frame = talloc_stackframe();

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
                status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	req = smb1_setfileinfo_send(frame,
				    ev,
				    cli,
				    fnum,
				    target_name,
				    info_level);
	if (req == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}

	status = smb1_setfileinfo_recv(req);

  fail:

	TALLOC_FREE(frame);
	return status;
}

static NTSTATUS smb1_setfileinfo_rename(struct cli_state *cli,
					uint16_t fnum,
					const char *target_name)
{
	return smb1_setfileinfo(cli,
				fnum,
				target_name,
				SMB_FILE_RENAME_INFORMATION);
}

/*
 * On Windows, rename using a file handle as source
 * is not supported.
 */

static bool test_smb1_setfileinfo_rename(struct cli_state *cli,
					 const char *src_dfs_name)
{
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	bool retval = false;

	/* First open the source file. */
	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   src_dfs_name,
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_STD_DELETE, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_OPEN, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d failed to open %s, %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			nt_errstr(status));
		goto out;
	}

	/*
	 * On Windows rename given a file handle returns
	 * NT_STATUS_UNSUCCESSFUL (not documented in MS-SMB).
	 */

	status = smb1_setfileinfo_rename(cli,
					 fnum,
					 "BAD\\BAD\\renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_UNSUCCESSFUL)) {
		printf("%s:%d SMB1 setfileinfo rename of %s -> %s should get "
			"NT_STATUS_UNSUCCESSFUL got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\hlink",
			nt_errstr(status));
		goto out;
	}

	/* Try with a non-DFS name - still gets NT_STATUS_UNSUCCESSFUL. */
	status = smb1_setfileinfo_rename(cli,
					 fnum,
					 "renamed_file");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_UNSUCCESSFUL)) {
		printf("%s:%d SMB1 setfileinfo rename of %s -> %s should get "
			"NT_STATUS_UNSUCCESSFUL got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"hlink",
			nt_errstr(status));
		goto out;
	}

	retval = true;

  out:

	if (fnum != (uint16_t)-1) {
		(void)smb1cli_close(cli->conn,
				    cli->timeout,
				    cli->smb1.pid,
				    cli->smb1.tcon,
				    cli->smb1.session,
				    fnum,
				    0); /* last_modified */
	}

	(void)smb1_dfs_delete(cli, "BAD\\BAD\\renamed_file");
	return retval;
}


static NTSTATUS smb1_setfileinfo_hardlink(struct cli_state *cli,
					  uint16_t fnum,
					  const char *target_name)
{
	return smb1_setfileinfo(cli,
				fnum,
				target_name,
				SMB_FILE_LINK_INFORMATION);
}

/*
 * On Windows, hardlink using a file handle as source
 * is not supported.
 */

static bool test_smb1_setfileinfo_hardlink(struct cli_state *cli,
					   const char *src_dfs_name)
{
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	bool retval = false;

	/* First open the source file. */
	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   src_dfs_name,
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_RIGHTS_FILE_READ, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_OPEN, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d failed to open %s, %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			nt_errstr(status));
		goto out;
	}

	/*
	 * On Windows hardlink given a file handle returns
	 * NT_STATUS_UNSUCCESSFUL (not documented in MS-SMB).
	 */

	status = smb1_setfileinfo_hardlink(cli,
					   fnum,
					   "BAD\\BAD\\hlink");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_UNSUCCESSFUL)) {
		printf("%s:%d SMB1 setfileinfo hardlink of %s -> %s should get "
			"NT_STATUS_UNSUCCESSFUL got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"BAD\\BAD\\hlink",
			nt_errstr(status));
		goto out;
	}

	/* Try with a non-DFS name - still gets NT_STATUS_UNSUCCESSFUL. */
	status = smb1_setfileinfo_hardlink(cli,
					 fnum,
					 "hlink");
	if (!NT_STATUS_EQUAL(status, NT_STATUS_UNSUCCESSFUL)) {
		printf("%s:%d SMB1 setfileinfo hardlink of %s -> %s should get "
			"NT_STATUS_UNSUCCESSFUL got %s\n",
			__FILE__,
			__LINE__,
			src_dfs_name,
			"hlink",
			nt_errstr(status));
		goto out;
	}

	retval = true;

  out:

	if (fnum != (uint16_t)-1) {
		(void)smb1cli_close(cli->conn,
				    cli->timeout,
				    cli->smb1.pid,
				    cli->smb1.tcon,
				    cli->smb1.session,
				    fnum,
				    0); /* last_modified */
	}

	(void)smb1_dfs_delete(cli, "BAD\\BAD\\hlink");
	return retval;
}

/*
 * According to:

 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/dc9978d7-6299-4c5a-a22d-a039cdc716ea
 *
 *  (Characters " \ / [ ] : | < > + = ; , * ?,
 *  and control characters in range 0x00 through
 *  0x1F, inclusive, are illegal in a share name)
 *
 * But Windows server only checks in DFS sharenames ':'. All other
 * share names are allowed.
 */

static bool test_smb1_dfs_sharenames(struct cli_state *cli,
				     const char *dfs_root_share_name,
				     struct timespec root_crtime)
{
	char test_path[20];
	const char *test_str = "/[]:|<>+=;,*?";
	const char *p;
	unsigned int i;
	bool crtime_matched = false;

	/* Setup template pathname. */
	memcpy(test_path, "\\SERVER\\X", 10);

	/* Test invalid control characters. */
	for (i = 1; i < 0x20; i++) {
		test_path[8] = i;
		crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 test_path);
		if (!crtime_matched) {
			return false;
		}
	}

	/* Test explicit invalid characters. */
	for (p = test_str; *p != '\0'; p++) {
		test_path[8] = *p;
		if (*p == ':') {
			/*
			 * Only ':' is treated as an INVALID sharename
			 * for a DFS SERVER\\SHARE path.
			 */
			struct timespec test_crtime = { 0 };
			NTSTATUS status = get_smb1_crtime(cli,
							 test_path,
							 &test_crtime);
			if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_NAME_INVALID)) {
				printf("%s:%d Open of %s should get "
					"NT_STATUS_OBJECT_NAME_INVALID, got %s\n",
					__FILE__,
					__LINE__,
					test_path,
					nt_errstr(status));
				return false;
			}
		} else {
			crtime_matched = smb1_crtime_matches(cli,
						 dfs_root_share_name,
						 root_crtime,
						 test_path);
			if (!crtime_matched) {
				return false;
			}
		}
	}
	return true;
}

/*
 * "Raw" test of SMB1 paths to a DFS share.
 * We must (mostly) use the lower level smb1cli_XXXX() interfaces,
 * not the cli_XXX() ones here as the ultimate goal is to fix our
 * cli_XXX() interfaces to work transparently over DFS.
 *
 * So here, we're testing the server code, not the client code.
 *
 * Passes cleanly against Windows.
 */

bool run_smb1_dfs_paths(int dummy)
{
	struct cli_state *cli = NULL;
	NTSTATUS status;
	bool dfs_supported = false;
	char *dfs_root_share_name = NULL;
	struct timespec root_crtime = { 0 };
	struct timespec test_crtime = { 0 };
	bool crtime_matched = false;
	bool retval = false;
	bool ok = false;
	bool equal = false;
	unsigned int i;
	uint16_t fnum = (uint16_t)-1;

	printf("Starting SMB1-DFS-PATHS\n");

	if (!torture_init_connection(&cli)) {
		return false;
	}

	if (!torture_open_connection(&cli, 0)) {
		return false;
	}

	/* Ensure this is a DFS share. */
	dfs_supported = smbXcli_conn_dfs_supported(cli->conn);
	if (!dfs_supported) {
		printf("Server %s does not support DFS\n",
			smbXcli_conn_remote_name(cli->conn));
		return false;
	}
	dfs_supported = smbXcli_tcon_is_dfs_share(cli->smb1.tcon);
	if (!dfs_supported) {
		printf("Share %s does not support DFS\n",
			cli->share);
		return false;
	}

	/* Start with an empty share. */
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\BAD");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\file");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\renamed_file");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\hlink");

	/*
	 * Create the "official" DFS share root name.
	 */
	dfs_root_share_name = talloc_asprintf(talloc_tos(),
					"\\%s\\%s",
					smbXcli_conn_remote_name(cli->conn),
					cli->share);
	if (dfs_root_share_name == NULL) {
		printf("Out of memory\n");
		return false;
	}

	/* Get the share root crtime. */
	status = get_smb1_crtime(cli,
				dfs_root_share_name,
				&root_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Failed to get crtime for share root %s, (%s)\n",
			__FILE__,
			__LINE__,
			dfs_root_share_name,
			nt_errstr(status));
		return false;
	}

	/*
	 * Test the Windows algorithm for parsing DFS names.
	 */
	/*
	 * A single "SERVER" element should open and match the share root.
	 */
	crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 smbXcli_conn_remote_name(cli->conn));
	if (!crtime_matched) {
		printf("%s:%d Failed to match crtime for %s\n",
			__FILE__,
			__LINE__,
			smbXcli_conn_remote_name(cli->conn));
		return false;
	}

	/* An "" (empty) server name should open and match the share root. */
	crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 "");
	if (!crtime_matched) {
		printf("%s:%d Failed to match crtime for %s\n",
			__FILE__,
			__LINE__,
			"");
		return false;
	}

	/*
	 * For SMB1 the server just strips off any number of leading '\\'
	 * characters. Show this is the case.
	 */
	for (i = 0; i < 10; i++) {
		char leading_backslash_name[20];
		leading_backslash_name[i] = '\\';
		memcpy(&leading_backslash_name[i+1],
			"SERVER",
			strlen("SERVER")+1);

		crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 leading_backslash_name);
		if (!crtime_matched) {
			printf("%s:%d Failed to match crtime for %s\n",
				__FILE__,
				__LINE__,
				leading_backslash_name);
			return false;
		}
	}

	/* A "BAD" server name should open and match the share root. */
	crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 "BAD");
	if (!crtime_matched) {
		printf("%s:%d Failed to match crtime for %s\n",
			__FILE__,
			__LINE__,
			"BAD");
		return false;
	}
	/*
	 * A "BAD\\BAD" server and share name should open
	 * and match the share root.
	 */
	crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 "BAD\\BAD");
	if (!crtime_matched) {
		printf("%s:%d Failed to match crtime for %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD");
		return false;
	}
	/*
	 * Trying to open "BAD\\BAD\\BAD" should get
	 * NT_STATUS_OBJECT_NAME_NOT_FOUND.
	 */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\BAD",
				&test_crtime);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_NAME_NOT_FOUND)) {
		printf("%s:%d Open of %s should get "
			"STATUS_OBJECT_NAME_NOT_FOUND, got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\BAD",
			nt_errstr(status));
		return false;
	}
	/*
	 * Trying to open "BAD\\BAD\\BAD\\BAD" should get
	 * NT_STATUS_OBJECT_PATH_NOT_FOUND.
	 */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\BAD\\BAD",
				&test_crtime);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_NOT_FOUND)) {
		printf("%s:%d Open of %s should get "
			"STATUS_OBJECT_NAME_NOT_FOUND, got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\BAD\\BAD",
			nt_errstr(status));
		return false;
	}
	/*
	 * Test for invalid pathname characters in the servername.
	 * They are ignored, and it still opens the share root.
	 */
	crtime_matched = smb1_crtime_matches(cli,
					 dfs_root_share_name,
					 root_crtime,
					 "::::");
	if (!crtime_matched) {
		printf("%s:%d Failed to match crtime for %s\n",
			__FILE__,
			__LINE__,
			"::::");
		return false;
	}

	/*
	 * Test for invalid pathname characters in the sharename.
	 * Invalid sharename characters should still be flagged as
	 * NT_STATUS_OBJECT_NAME_INVALID. It turns out only ':'
	 * is considered an invalid sharename character.
	 */
	ok = test_smb1_dfs_sharenames(cli,
				      dfs_root_share_name,
				      root_crtime);
	if (!ok) {
		return false;
	}

	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   "BAD\\BAD\\file",
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_STD_DELETE |
					SEC_FILE_READ_DATA|
					SEC_FILE_READ_ATTRIBUTE, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_CREATE, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d smb1cli_ntcreatex on %s returned %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\file",
			nt_errstr(status));
		return false;
	}

	/* Close "file" handle. */
	(void)smb1cli_close(cli->conn,
			    cli->timeout,
			    cli->smb1.pid,
			    cli->smb1.tcon,
			    cli->smb1.session,
			    fnum,
			    0); /* last_modified */
	fnum = (uint16_t)-1;

	/*
	 * Trying to open "BAD\\BAD\\file" should now get
	 * a valid crtime.
	 */
	status = get_smb1_crtime(cli,
				"BAD\\BAD\\file",
				&test_crtime);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d Open of %s should succeed "
			"got %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\file",
			nt_errstr(status));
		goto err;
	}

	/*
	 * This crtime must be different from the root_crtime.
	 * This checks we're actually correctly reading crtimes
	 * from the filesystem.
	 */
	equal = (timespec_compare(&test_crtime, &root_crtime) == 0);
	if (equal) {
		printf("%s:%d Error. crtime of %s must differ from "
			"root_crtime\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\file");
		goto err;
	}

	/*
	 * Test different SMB1 renames
	 * and hard links.
	 */

	/* SMBmv only does rename. */
	ok = test_smb1_mv(cli,
			  "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_setpathinfo_rename(cli,
					  "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_setpathinfo_hardlink(cli,
					    "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_setfileinfo_rename(cli,
					  "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_setfileinfo_hardlink(cli,
					    "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_ntrename_rename(cli,
				       "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	ok = test_smb1_ntrename_hardlink(cli,
					  "BAD\\BAD\\file");
	if (!ok) {
		goto err;
	}

	retval = true;

  err:

	if (fnum != (uint16_t)-1) {
		(void)smb1cli_close(cli->conn,
				    cli->timeout,
				    cli->smb1.pid,
				    cli->smb1.tcon,
				    cli->smb1.session,
				    fnum,
				    0); /* last_modified */
	}

	/* Delete anything we made. */
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\BAD");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\file");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\renamed_file");
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\hlink");
	return retval;
}

/*
 * SMB1 Findfirst. This is a minimal implementation
 * that expects all filename returns in one packet.
 * We're only using this to test the search DFS pathname
 * parsing.
 */

/****************************************************************************
 Calculate a safe next_entry_offset.
****************************************************************************/

static size_t calc_next_entry_offset(const uint8_t *base,
				     const uint8_t *pdata_end)
{
	size_t next_entry_offset = (size_t)PULL_LE_U32(base,0);

	if (next_entry_offset == 0 ||
			base + next_entry_offset < base ||
			base + next_entry_offset > pdata_end) {
		next_entry_offset = pdata_end - base;
	}
	return next_entry_offset;
}

static size_t get_filename(TALLOC_CTX *ctx,
			   struct cli_state *cli,
			   const uint8_t *base_ptr,
			   uint16_t recv_flags2,
			   const uint8_t *p,
			   const uint8_t *pdata_end,
			   struct file_info *finfo)
{
	size_t ret = 0;
	const uint8_t *base = p;
	size_t namelen = 0;
	size_t slen = 0;

        ZERO_STRUCTP(finfo);

	if (pdata_end - base < 94) {
		return pdata_end - base;
	}
	p += 4; /* next entry offset */
	p += 4; /* fileindex */
	/* Offset zero is "create time", not "change time". */
	p += 8;
	finfo->atime_ts = interpret_long_date((const char *)p);
	p += 8;
	finfo->mtime_ts = interpret_long_date((const char *)p);
	p += 8;
	finfo->ctime_ts = interpret_long_date((const char *)p);
	p += 8;
	finfo->size = PULL_LE_U64(p, 0);
	p += 8;
	p += 8; /* alloc size */
	finfo->attr = PULL_LE_U32(p, 0);
	p += 4;
	namelen = PULL_LE_U32(p, 0);
	p += 4;
	p += 4; /* EA size */
	slen = PULL_LE_U8(p, 0);
	if (slen > 24) {
		/* Bad short name length. */
		return pdata_end - base;
	}
	p += 2;
	ret = pull_string_talloc(ctx,
				 base_ptr,
				 recv_flags2,
				 &finfo->short_name,
				 p,
				 slen,
				 STR_UNICODE);
	if (ret == (size_t)-1) {
		return pdata_end - base;
	}
	p += 24; /* short name */
	if (p + namelen < p || p + namelen > pdata_end) {
		return pdata_end - base;
	}
	ret = pull_string_talloc(ctx,
				 base_ptr,
				 recv_flags2,
				 &finfo->name,
				 p,
				 namelen,
				 0);
	if (ret == (size_t)-1) {
		return pdata_end - base;
	}
	return calc_next_entry_offset(base, pdata_end);
}

/* Single shot SMB1 TRANS2 FindFirst. */

static NTSTATUS smb1_findfirst(TALLOC_CTX *mem_ctx,
			       struct cli_state *cli,
			       const char *search_name,
			       struct file_info **names,
			       size_t *num_names)
{
	NTSTATUS status;
	uint16_t setup[1];
	uint8_t *param = NULL;
	uint16_t recv_flags2 = 0;
	uint8_t *rparam = NULL;
	uint32_t num_rparam = 0;
	uint8_t *rdata = NULL;
	uint32_t num_rdata = 0;
	uint16_t num_names_returned = 0;
	struct file_info *finfo = NULL;
	uint8_t *p2 = NULL;
	uint8_t *data_end = NULL;
	uint16_t i = 0;

	PUSH_LE_U16(&setup[0], 0, TRANSACT2_FINDFIRST);

	param = talloc_array(mem_ctx, uint8_t, 12);
	if (param == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	PUSH_LE_U16(param, 0, FILE_ATTRIBUTE_DIRECTORY |
		FILE_ATTRIBUTE_SYSTEM |
		FILE_ATTRIBUTE_HIDDEN);
	PUSH_LE_U16(param, 2, 1366); /* max_matches */
	PUSH_LE_U16(param, 4, FLAG_TRANS2_FIND_CLOSE_IF_END);
	PUSH_LE_U16(param, 6, SMB_FIND_FILE_BOTH_DIRECTORY_INFO); /* info_level */

	param = trans2_bytes_push_str(param,
				      smbXcli_conn_use_unicode(cli->conn),
				      search_name,
				      strlen(search_name)+1,
				      NULL);
	if (param == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * A one shot SMB1 findfirst will be enough to
	 * return ".", "..", and "file".
	 */
	status = cli_trans(mem_ctx,
			   cli,
			   SMBtrans2, /* cmd */
			   NULL, /* pipe_name */
			   0, /* fid */
			   0, /* function */
			   0, /* flags */
			   &setup[0],
			   1, /* num_setup uint16_t words */
			   0, /* max returned setup */
			   param,
			   talloc_get_size(param), /* num_param */
			   10, /* max returned param */
			   NULL, /* data */
			   0, /* num_data */
			   SMB_BUFFER_SIZE_MAX, /* max retured data */
			   /* Return values from here on.. */
			   &recv_flags2, /* recv_flags2 */
			   NULL, /* rsetup */
			   0, /* min returned rsetup */
			   NULL, /* num_rsetup */
			   &rparam,
			   6, /* min returned rparam */
			   &num_rparam, /* number of returned rparam */
			   &rdata,
			   0, /* min returned rdata */
			   &num_rdata);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	num_names_returned = PULL_LE_U16(rparam, 2);

        finfo = talloc_array(mem_ctx, struct file_info, num_names_returned);
	if (param == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	p2 = rdata;
        data_end = rdata + num_rdata;

	for (i = 0; i < num_names_returned; i++) {
		if (p2 >= data_end) {
			break;
		}
		if (i == num_names_returned - 1) {
			/* Last entry - fixup the last offset length. */
			PUSH_LE_U32(p2, 0, PTR_DIFF((rdata + num_rdata), p2));
		}

		p2 += get_filename(mem_ctx,
				   cli,
				   rdata,
				   recv_flags2,
				   p2,
				   data_end,
				   &finfo[i]);

		if (finfo->name == NULL) {
			printf("%s:%d Unable to parse name from listing "
				"of %s, position %u\n",
				__FILE__,
				__LINE__,
				search_name,
				(unsigned int)i);
			return NT_STATUS_INVALID_NETWORK_RESPONSE;
                }
	}
	*num_names = i;
	*names = finfo;
	return NT_STATUS_OK;
}

/*
 * Test a specific SMB1 findfirst path to see if it
 * matches a given file array.
 */
static bool test_smb1_findfirst_path(struct cli_state *cli,
				     const char *search_path,
				     struct file_info *root_finfo,
				     size_t num_root_finfo)
{
	size_t i = 0;
	size_t num_finfo = 0;
	struct file_info *finfo = NULL;
	NTSTATUS status;

	status = smb1_findfirst(talloc_tos(),
				cli,
				search_path,
				&finfo,
				&num_finfo);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d smb1findfirst on %s returned %s\n",
			__FILE__,
			__LINE__,
			search_path,
			nt_errstr(status));
		return false;
	}

	if (num_finfo != num_root_finfo) {
		printf("%s:%d On %s, num_finfo = %zu, num_root_finfo = %zu\n",
			__FILE__,
			__LINE__,
			search_path,
			num_finfo,
			num_root_finfo);
		return false;
	}
	for (i = 0; i < num_finfo; i++) {
		bool match = strequal_m(finfo[i].name,
					root_finfo[i].name);
		if (!match) {
			printf("%s:%d Missmatch. For %s, at position %zu, "
			       "finfo[i].name = %s, "
			       "root_finfo[i].name = %s\n",
				__FILE__,
				__LINE__,
				search_path,
				i,
				finfo[i].name,
				root_finfo[i].name);
			return false;
		}
	}
	TALLOC_FREE(finfo);
	return true;
}

/*
 * "Raw" test of doing a SMB1 findfirst to a DFS share.
 * We must (mostly) use the lower level smb1cli_XXXX() interfaces,
 * not the cli_XXX() ones here as the ultimate goal is to fix our
 * cli_XXX() interfaces to work transparently over DFS.
 *
 * So here, we're testing the server code, not the client code.
 *
 * Passes cleanly against Windows.
 */

bool run_smb1_dfs_search_paths(int dummy)
{
	struct cli_state *cli = NULL;
	NTSTATUS status;
	bool dfs_supported = false;
	struct file_info *root_finfo = NULL;
	size_t num_root_finfo = 0;
	bool retval = false;
	bool ok = false;
	uint16_t fnum = (uint16_t)-1;

	printf("Starting SMB1-DFS-SEARCH-PATHS\n");

	if (!torture_init_connection(&cli)) {
		return false;
	}

	if (!torture_open_connection(&cli, 0)) {
		return false;
	}

	/* Ensure this is a DFS share. */
	dfs_supported = smbXcli_conn_dfs_supported(cli->conn);
	if (!dfs_supported) {
		printf("Server %s does not support DFS\n",
			smbXcli_conn_remote_name(cli->conn));
		return false;
	}
	dfs_supported = smbXcli_tcon_is_dfs_share(cli->smb1.tcon);
	if (!dfs_supported) {
		printf("Share %s does not support DFS\n",
			cli->share);
		return false;
	}

	/* Start clean. */
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\file");

	/* Create a test file to search for. */
	status = smb1cli_ntcreatex(cli->conn,
				   cli->timeout,
				   cli->smb1.pid,
				   cli->smb1.tcon,
				   cli->smb1.session,
				   "BAD\\BAD\\file",
				   OPLOCK_NONE, /* CreatFlags */
				   0, /* RootDirectoryFid */
				   SEC_STD_SYNCHRONIZE|
					SEC_STD_DELETE |
					SEC_FILE_READ_DATA|
					SEC_FILE_READ_ATTRIBUTE, /* DesiredAccess */
				   0, /* AllocationSize */
				   FILE_ATTRIBUTE_NORMAL, /* FileAttributes */
				   FILE_SHARE_READ|
					FILE_SHARE_WRITE|
					FILE_SHARE_DELETE, /* ShareAccess */
				   FILE_CREATE, /* CreateDisposition */
				   0, /* CreateOptions */
				   2, /* ImpersonationLevel */
				   0, /* SecurityFlags */
				   &fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d smb1cli_ntcreatex on %s returned %s\n",
			__FILE__,
			__LINE__,
			"BAD\\BAD\\file",
			nt_errstr(status));
		return false;
	}

	/* Close "file" handle. */
	(void)smb1cli_close(cli->conn,
			    cli->timeout,
			    cli->smb1.pid,
			    cli->smb1.tcon,
			    cli->smb1.session,
			    fnum,
			    0); /* last_modified */
	fnum = (uint16_t)-1;

	/* Get the list of files in the share. */
	status = smb1_findfirst(talloc_tos(),
				cli,
				"SERVER\\SHARE\\*",
				&root_finfo,
				&num_root_finfo);
	if (!NT_STATUS_IS_OK(status)) {
		printf("%s:%d smb1findfirst on %s returned %s\n",
			__FILE__,
			__LINE__,
			"SERVER\\SHARE\\*",
			nt_errstr(status));
		return false;
	}

	/*
	 * Try different search names. They should
	 * all match the root directory list.
	 */
	ok = test_smb1_findfirst_path(cli,
				      "\\SERVER\\SHARE\\*",
				      root_finfo,
				      num_root_finfo);
	if (!ok) {
		goto err;
	}

	ok = test_smb1_findfirst_path(cli,
				      "*",
				      root_finfo,
				      num_root_finfo);
	if (!ok) {
		goto err;
	}
	ok = test_smb1_findfirst_path(cli,
				      "\\*",
				      root_finfo,
				      num_root_finfo);
	if (!ok) {
		goto err;
	}
	ok = test_smb1_findfirst_path(cli,
				      "\\SERVER\\*",
				      root_finfo,
				      num_root_finfo);
	if (!ok) {
		goto err;
	}
	retval = true;

  err:

	if (fnum != (uint16_t)-1) {
		(void)smb1cli_close(cli->conn,
				    cli->timeout,
				    cli->smb1.pid,
				    cli->smb1.tcon,
				    cli->smb1.session,
				    fnum,
				    0); /* last_modified */
	}

	/* Delete anything we made. */
	(void)smb1_dfs_delete(cli, "BAD\\BAD\\file");
	return retval;
}
