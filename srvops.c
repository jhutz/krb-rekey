/*
 * Copyright (c) 2008-2009 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/signal.h>
#include <netdb.h>
#include <arpa/inet.h>

#define SESS_PRIVATE
#define NEED_KRB5
#define NEED_GSSAPI
#define NEED_SQLITE
#include "rekeysrv-locl.h"
#include "rekey-locl.h"
#include "protocol.h"
#include "memmgt.h"

#ifdef HEADER_GSSAPI_GSSAPI
#include <gssapi/gssapi_krb5.h>
#endif

#define USE_KADM5_API_VERSION 2
#include <kadm5/admin.h>
#ifdef HAVE_KADM5_KADM5_ERR_H
#include <kadm5/kadm5_err.h>
#endif

static krb5_enctype des_enctypes[] = {
  ENCTYPE_DES_CBC_CRC,
  ENCTYPE_NULL
};

static krb5_enctype cur_enctypes[] = {
  ENCTYPE_DES_CBC_CRC,
  ENCTYPE_DES3_CBC_SHA1,
  ENCTYPE_NULL
};

static krb5_enctype future_enctypes[] = {
  ENCTYPE_DES_CBC_CRC,
  ENCTYPE_DES3_CBC_SHA1,
#ifdef ENCTYPE_AES128_CTS_HMAC_SHA1_96
  ENCTYPE_AES128_CTS_HMAC_SHA1_96,
#endif
#ifdef ENCTYPE_AES356_CTS_HMAC_SHA1_96
  ENCTYPE_AES256_CTS_HMAC_SHA1_96,
#endif
#ifdef ENCTYPE_ARCFOUR_HMAC
  ENCTYPE_ARCFOUR_HMAC,
#endif
  ENCTYPE_NULL
};

#ifdef THE_FUTURE_IS_NOW
#define cur_enctypes future_enctypes
#endif

static void check_authz(struct rekey_session *sess) 
{
  char *realm;
#if defined(KRB5_PRINCIPAL_HEIMDAL_STYLE)
  const char  *princ_realm, *c1, *c2, *c3;
#elif defined (KRB5_PRINCIPAL_MIT_STYLE)
  krb5_data *princ_realm, *c1, *c2;
#else
#error Cannot figure out how krb5_principals objects work
#endif
  if (krb5_get_default_realm(sess->kctx, &realm))
    return;
#if defined(KRB5_PRINCIPAL_HEIMDAL_STYLE)

  princ_realm = krb5_principal_get_realm(sess->kctx, sess->princ); 
  if (!princ_realm || strncmp(princ_realm , realm, strlen(realm)))
     goto out;
  c1 = krb5_principal_get_comp_string(sess->kctx, sess->princ, 0);
  c2 = krb5_principal_get_comp_string(sess->kctx, sess->princ, 1);
  c3 = krb5_principal_get_comp_string(sess->kctx, sess->princ, 2);

  if (c1 && c2 && !c3 && !strncmp(c1, "host", 5)) {
    sess->is_host = 1;
    sess->hostname=malloc(strlen(c2)+1);
    if (sess->hostname)
      strcpy(sess->hostname, c2);
    else /* mark not a valid host, since we don't have its identification */
      sess->is_host = 0;
    goto out;
  }
  
  if (c1 && c2 && !c3 && !strncmp(c2, "admin", 5)) {
    sess->is_admin = 1;
    goto out;
  }

 out:
  krb5_xfree(realm);
#elif defined (KRB5_PRINCIPAL_MIT_STYLE)

  princ_realm = krb5_princ_realm(sess->kctx, sess->princ); 
  if (!princ_realm || strncmp(princ_realm->data , realm, strlen(realm)))
     goto out;
  if (krb5_princ_size(sess->kctx, sess->princ) != 2)
    goto out;
  c1 = krb5_princ_component(sess->kctx, sess->princ, 0);
  c2 = krb5_princ_component(sess->kctx, sess->princ, 1);

  if (!strncmp(c1->data, "host", 5)) {
    sess->is_host = 1;
    sess->hostname=malloc(c2->length+1);
    if (sess->hostname) {
      strncpy(sess->hostname, c2->data, c2->length);
      sess->hostname[c2->length]=0;
    } else /* mark not a valid host, since we don't have its identification */
      sess->is_host = 0;
    goto out;
  }
  
  if (!strncmp(c2->data, "admin", 5)) {
    sess->is_admin = 1;
    goto out;
  }

 out:
  krb5_free_default_realm(sess->kctx, realm);
#endif
}

static void s_auth(struct rekey_session *sess, mb_t buf) {
  OM_uint32 maj, min, tmin, rflag;
  gss_buffer_desc in, out, outname;
  unsigned int f;
  int gss_more_accept=0, gss_more_init=0;
  unsigned char *p;
  krb5_error_code rc;
  
  if (sess->authstate) {
    send_error(sess, ERR_BADOP, "Authentication already complete");
    return;
  }
  if (krb5_init_context(&sess->kctx)) {
      
      send_fatal(sess, ERR_OTHER, "Internal kerberos error on server");
      fatal("Authentication failed: krb5_init_context failed");
  }  
  reset_cursor(buf);
  if (buf_getint(buf, &f))
    goto badpkt;
  if (f & AUTHFLAG_MORE)
    gss_more_init = 1;
  if (buf_getint(buf, (unsigned int *)&in.length))
    goto badpkt;
  in.value = buf->cursor;
  memset(&out, 0, sizeof(out));
  maj = gss_accept_sec_context(&min, &sess->gctx, GSS_C_NO_CREDENTIAL,
			       &in, GSS_C_NO_CHANNEL_BINDINGS,
			       &sess->name, &sess->mech, &out, &rflag, NULL,
			       NULL);
  if (GSS_ERROR(maj)) {
    if (out.length) {
      send_gss_token(sess, RESP_AUTHERR, 0, &out);
      gss_release_buffer(&tmin, &out);
      prt_gss_error(sess->mech, maj, min);
    } else {
      send_gss_error(sess, sess->mech, maj, min);
    }
    if (sess->gctx != GSS_C_NO_CONTEXT)
      gss_delete_sec_context(&tmin, &sess->gctx, GSS_C_NO_BUFFER);
    return;
  }
  if (maj & GSS_S_CONTINUE_NEEDED) {
    gss_more_accept=1;
    if (out.length == 0) {
      send_fatal(sess, ERR_OTHER, "Internal gss error on server");
      fatal("Authentication failed: not sending a gss token but expects a reply");
    }
  }

  if (out.length && gss_more_init == 0) {
    send_fatal(sess, ERR_OTHER, "Internal gss error on server");
    fatal("Authentication failed: would send a gss token when remote does not expect one");
  }


  if (gss_more_accept == 0) {
    unsigned short oidl;
    if ((~rflag) & (GSS_C_MUTUAL_FLAG|GSS_C_INTEG_FLAG)) {
      send_fatal(sess, ERR_AUTHN, "GSSAPI mechanism does not provide data integrity services");
      fatal("GSSAPI mechanism does not provide data integrity services");
    }   
    maj = gss_export_name(&min, sess->name, &outname);
    if (GSS_ERROR(maj)) {
      prt_gss_error(sess->mech, maj, min);
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (cannot export name from GSSAPI)");
      fatal("Cannot parse authenticated name (cannot export name from GSSAPI)");
    }
    /* check for minimum length and correct token header */
    if (outname.length < 6 || memcmp(outname.value, "\x04\x01", 2)) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (it is not a valid exported name)");
      fatal("Cannot parse authenticated name (it is not a valid exported name)");
    }
    p = outname.value;
    p += 2;
    /* extract oid wrapper length */
    oidl = (p[0] << 8) + p[1];
    p+=2;
    /* check for oid length, valid oid tag, and correct oid length. 
       (this isn't really general - a sufficiently long oid would break this,
       even if valid) */
    if (outname.length < 4 + oidl || *p++ != 0x6 || *p >= 0x80 || *p++ != oidl - 2 ) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (it is not a valid exported name)");
      fatal("Cannot parse authenticated name (it is not a valid exported name)");
    }
    oidl -= 2;
    /* check for the krb5 mechanism oid */
    if (gss_mech_krb5->length != oidl || 
	memcmp(p, gss_mech_krb5->elements, oidl)) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (it is not a kerberos name)");
      fatal("Cannot parse authenticated name (it is not a kerberos name)");
    }
    /* skip oid */
    p+=oidl;
    if (buf_setlength(buf, outname.length) ||
        buf_putdata(buf, outname.value, outname.length)) {
      send_fatal(sess, ERR_OTHER, "Internal error on server");
      fatal("internal error: cannot copy name structure");
    }      
    /* skip over the header we already parsed */
    set_cursor(buf, p - (unsigned char *)outname.value);
    gss_release_buffer(&tmin, &outname);
    if (buf_getint(buf, &f)) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (unknown error)");
      fatal("Cannot parse authenticated name (buffer is too short)");
    }
    sess->plain_name=malloc(f + 1);
    if (!sess->plain_name) {
      send_fatal(sess, ERR_OTHER, "Internal error on server");
      fatal("Cannot allocate memory");
    }
    if (buf_getdata(buf, sess->plain_name, f)) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (unknown error)");
      fatal("Cannot parse authenticated name (buffer is broken [name length=%d, input buffer size=%d])", f, outname.length - (p - (unsigned char *)outname.value) - 4);
    }
    sess->plain_name[f]=0;
    if ((rc=krb5_parse_name(sess->kctx, sess->plain_name, &sess->princ))) {
      send_fatal(sess, ERR_AUTHN, "Cannot parse authenticated name (unknown error)");
      fatal("Cannot parse authenticated name (kerberos error %s)", krb5_get_err_text(sess->kctx, rc));
    }
    sess->authstate=1;
    check_authz(sess);
    prtmsg("Authenticated as %s (host? %d admin? %d)", sess->plain_name,
           sess->is_host, sess->is_admin);
  }
  if (out.length) {
    send_gss_token(sess, RESP_AUTH, gss_more_accept, &out);
    gss_release_buffer(&tmin, &out);
  } else {
    do_send(sess->ssl, RESP_OK, NULL);
  }
  return;
 badpkt:
  send_error(sess, ERR_BADREQ, "Packet was too short for opcode");
  return;
}
static void s_autherr(struct rekey_session *sess, mb_t buf) 
{
  OM_uint32 maj, min;
  gss_buffer_desc in, out;
  unsigned int f;

  if (sess->authstate) {
    send_error(sess, ERR_BADOP, "Authentication already complete");
    return;
  }
  
  if (buf_getint(buf, &f))
    goto badpkt;
  if (buf_getint(buf, (unsigned int *)&in.length))
    goto badpkt;
  in.value = buf->cursor;
  memset(&out, 0, sizeof(out));
  maj = gss_accept_sec_context(&min, &sess->gctx, GSS_C_NO_CREDENTIAL,
			       &in, GSS_C_NO_CHANNEL_BINDINGS,
			       &sess->name, &sess->mech, &out, NULL, NULL,
			       NULL);
  if (GSS_ERROR(maj)) {
    prt_gss_error(sess->mech, maj, min);
  } else {
    prtmsg("got autherr packet from client, but no GSSAPI error inside");
  }
  if (out.length)
    gss_release_buffer(&min, &out);
  if (sess->gctx)
    gss_delete_sec_context(&min, &sess->gctx, GSS_C_NO_BUFFER);
  
  do_send(sess->ssl, RESP_OK, NULL);
  do_finalize(sess);
  exit(1);
 badpkt:
  send_error(sess, ERR_BADREQ, "Packet was too short for opcode");
  return;
}

static void s_authchan(struct rekey_session *sess, mb_t buf) 
{
  OM_uint32 maj, min, qop;
  gss_buffer_desc in, out;
  size_t flen;
  unsigned char *p;

  if (sess->authstate == 0) {
    send_error(sess, ERR_AUTHZ, "Operation not allowed on unauthenticated connection");
    return;
  }
  if (sess->authstate == 2) {
    send_error(sess, ERR_BADOP, "Authentication already complete");
    return;
  }

 flen = SSL_get_finished(sess->ssl, NULL, 0);
 if (flen == 0) {
   send_fatal(sess, ERR_AUTHN, "ssl finished message not available");
   fatal("Cannot authenticate: ssl finished message not available");
 }    
 in.length = 2 * flen;
 in.value = malloc(in.length);
 if (in.value == NULL) {
   send_fatal(sess, ERR_AUTHN, "Internal error; out of memory");
   fatal("Cannot authenticate: memory allocation failed: %s",
         strerror(errno));
 }
 p=in.value;
 if (flen != SSL_get_peer_finished(sess->ssl, p, flen)) {
   send_fatal(sess, ERR_AUTHN, "ssl finished message not available");
   fatal("Cannot authenticate: ssl finished message not available or size changed(!)");
 }    
 p+=flen;
 if (flen != SSL_get_finished(sess->ssl, p, flen)) {
   send_fatal(sess, ERR_AUTHN, "ssl finished message not available");
   fatal("Cannot authenticate: ssl finished message not available or size changed(!)");
 }

 out.length = buf->length;
 out.value = buf->value;
 
 maj = gss_verify_mic(&min, sess->gctx, &in, &out, &qop);
 if (maj == GSS_S_BAD_SIG) {
   send_fatal(sess, ERR_AUTHN, "Channel binding verification failed");
   fatal("channel binding verification failed (signature does not match)");
 }
 if (GSS_ERROR(maj)) {
   send_gss_error(sess, sess->mech, maj, min);
   free(in.value);
   return;
 }
 
 p=in.value;
 if (flen != SSL_get_finished(sess->ssl, p, flen)) {
   send_fatal(sess, ERR_AUTHN, "ssl finished message not available");
   fatal("Cannot authenticate: ssl finished message not available or size changed(!)");
 }    
 p+=flen;
 if (flen != SSL_get_peer_finished(sess->ssl, p, flen)) {
   send_fatal(sess, ERR_AUTHN, "ssl finished message not available");
   fatal("Cannot authenticate: ssl finished message not available or size changed(!)");
 }
 memset(&out, 0, sizeof(out));
 maj = gss_get_mic(&min, sess->gctx, GSS_C_QOP_DEFAULT, &in, &out);
 free(in.value);
 if (GSS_ERROR(maj)) {
   send_gss_error(sess, sess->mech, maj, min);
   exit(1);
 }
 if (buf_setlength(buf, out.length) ||
     buf_putdata(buf, out.value, out.length)) {
    send_fatal(sess, ERR_OTHER, "Internal error on server");
    fatal("internal error: cannot pack channel binding structure");
 }
 
 do_send(sess->ssl, RESP_AUTHCHAN, buf);
 gss_release_buffer(&min, &out);
 sess->authstate = 2;
#if 0
 {
   SSL_SESSION *ssls = SSL_get_session(sess->ssl);
   char sslid[2 * SSL_MAX_SSL_SESSION_ID_LENGTH + 4], *p;
   int i;
   sprintf(sslid, "0x");
   p=&sslid[2];
   for (i=0; i < ssls->session_id_length; i++) {
     sprintf(p, "%02x", ssls->session_id[i]);
     p+=2;
   }
   prtmsg("Authentication bound to SSL %s", sslid);
 }
#else
 prtmsg("Channel bindings sucessfully verified");
#endif
}

static void s_newreq(struct rekey_session *sess, mb_t buf) 
{
  char *principal=NULL;
  char **hostnames=NULL;
  int desonly;
  unsigned int l, n, flag;
  int i, rc;
  sqlite3_stmt *ins=NULL;
  int dbaction=0;
  sqlite_int64 princid;
  krb5_enctype *pEtype;
  krb5_keyblock keyblock;
  krb5_error_code kc;
  int kvno, match;
  void *kadm_handle=NULL;
  kadm5_config_params kadm_param;
  char *realm;
#if defined(KRB5_PRINCIPAL_HEIMDAL_STYLE)
  const char  *princ_realm;
#elif defined(KRB5_PRINCIPAL_MIT_STYLE)
  krb5_data *princ_realm;
#endif
  krb5_principal target=NULL;
  kadm5_principal_ent_rec ke;
  
  if (sess->is_admin == 0) {
    send_error(sess, ERR_AUTHZ, "Not authorized (you must be an administrator)");
    return;
  }
  if (buf_getint(buf, &l))
    goto badpkt;
  principal = malloc(l + 1);
  if (!principal)
    goto memerr;
  if (buf_getdata(buf, principal, l))
    goto badpkt;
  principal[l]=0;
  rc = krb5_parse_name(sess->kctx, principal, &target);
  if (rc) {
    prtmsg("Cannot parse target name %s (kerberos error %s)", principal, krb5_get_err_text(sess->kctx, rc));
    send_error(sess, ERR_BADREQ, "Bad principal name");
    goto freeall;
  }

  if (buf_getint(buf, &flag))
    goto badpkt;
  if (flag != 0 && flag != REQFLAG_DESONLY) {
    send_error(sess, ERR_BADREQ, "Invalid flags specified");
    goto freeall;
  }
  desonly=0;
  if (flag == REQFLAG_DESONLY)
    desonly=1;
  if (buf_getint(buf, &n))
    goto badpkt;
  hostnames=calloc(n, sizeof(char *));
  if (!hostnames)
    goto memerr;
  for (i=0; i < n; i++) {
    if (buf_getint(buf, &l))
      goto badpkt;
    hostnames[i] = malloc(l + 1);
    if (!hostnames[i])
      goto memerr;
    if (buf_getdata(buf, hostnames[i], l))
      goto badpkt;
    hostnames[i][l]=0;
  }
  rc=krb5_get_default_realm(sess->kctx, &realm);
  if (rc) {
    prtmsg("Unable to get default realm: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }
#if defined(KRB5_PRINCIPAL_HEIMDAL_STYLE)

  princ_realm = krb5_principal_get_realm(sess->kctx, sess->princ); 
  if (!princ_realm || strncmp(princ_realm , realm, strlen(realm))) {
    send_error(sess, ERR_AUTHZ, "Requested principal is in wrong realm");
    goto freeall;
  }

#elif defined(KRB5_PRINCIPAL_MIT_STYLE)

  princ_realm = krb5_princ_realm(sess->kctx, sess->princ); 
  if (!princ_realm || strncmp(princ_realm->data , realm, strlen(realm))) {
    send_error(sess, ERR_AUTHZ, "Requested principal is in wrong realm");
    goto freeall;
  }
#endif

  if (sql_init(sess))
    goto dberrnomsg;

  rc = sqlite3_prepare_v2(sess->dbh, 
                          "SELECT id from principals where name=?",
                          -1, &ins, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(ins, 1, principal, strlen(principal), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  match=0;
  while (SQLITE_ROW == sqlite3_step(ins))
    match++;
  rc = sqlite3_finalize(ins);
  ins=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  if (match) {
    send_error(sess, ERR_OTHER, "Rekey for this principal already in progress");
    goto freeall;
  }

  kadm_param.mask = KADM5_CONFIG_REALM;
  kadm_param.realm = realm;
  memset(&ke, 0, sizeof(ke));
#ifdef HAVE_KADM5_INIT_WITH_SKEY_CTX
  rc = kadm5_init_with_skey_ctx(sess->kctx, 
			    "rekey/admin", NULL, KADM5_ADMIN_SERVICE,
			    &kadm_param, KADM5_STRUCT_VERSION, 
			    KADM5_API_VERSION_2, &kadm_handle);
#else
  rc = kadm5_init_with_skey("rekey/admin", NULL, KADM5_ADMIN_SERVICE,
			    &kadm_param, KADM5_STRUCT_VERSION, 
			    KADM5_API_VERSION_2, NULL, &kadm_handle);
#endif
  if (rc) {
    prtmsg("Unable to initialize kadm5 library: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }

  rc = kadm5_get_principal(kadm_handle, target, &ke, KADM5_KVNO);
  if (rc) {
    if (rc == KADM5_UNK_PRINC) {
      prtmsg("Principal %s does not exist", principal);
      send_error(sess, ERR_NOTFOUND, "Requested principal does not exist");
      goto freeall;
    }
    prtmsg("Unable to initialize kadm5 library: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }
  kvno = ke.kvno + 1;

  if (sql_begin_trans(sess))
    goto dberrnomsg;
  dbaction=-1;
  
  rc = sqlite3_prepare_v2(sess->dbh, 
			  "INSERT INTO principals (name, kvno) VALUES (?, ?);",
			  -1, &ins, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(ins, 1, principal, strlen(principal), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int(ins, 2, kvno);
  if (rc != SQLITE_OK)
    goto dberr;
  sqlite3_step(ins);    
  rc = sqlite3_finalize(ins);
  ins=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  princid  = sqlite3_last_insert_rowid(sess->dbh);
  
  
  rc = sqlite3_prepare_v2(sess->dbh, 
			  "INSERT INTO acl (principal, hostname, status) VALUES (?, ?, 0);",
			  -1, &ins, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  for (i=0; i < n; i++) {  
    rc = sqlite3_bind_int64(ins, 1, princid);
    if (rc != SQLITE_OK)
      goto dberr;
    rc = sqlite3_bind_text(ins, 2, hostnames[i], 
                           strlen(hostnames[i]), SQLITE_STATIC);
    if (rc != SQLITE_OK)
      goto dberr;
    sqlite3_step(ins);    
    rc = sqlite3_reset(ins);
    if (rc != SQLITE_OK)
      goto dberr;
  }
  rc = sqlite3_finalize(ins);
  ins=NULL;
  if (rc != SQLITE_OK)
    goto dberr;

  if (desonly)
    pEtype=des_enctypes;
  else
    pEtype=cur_enctypes;
  rc = sqlite3_prepare_v2(sess->dbh, 
			  "INSERT INTO keys (principal, enctype, key) VALUES (?, ?, ?);",
			  -1, &ins, NULL);
  for (;*pEtype != ENCTYPE_NULL; pEtype++) {
    kc = krb5_generate_random_keyblock(sess->kctx, *pEtype, &keyblock);
    if (kc) {
      prtmsg("Cannot generate key for enctype %d (kerberos error %s)", 
             *pEtype, krb5_get_err_text(sess->kctx, kc));
      goto interr;
    }
    rc = sqlite3_bind_blob(ins, 3, Z_keydata(&keyblock), 
                           Z_keylen(&keyblock), SQLITE_TRANSIENT);
    krb5_free_keyblock_contents(sess->kctx, &keyblock);
    if (rc != SQLITE_OK)
      goto dberr;
    rc = sqlite3_bind_int64(ins, 1, princid);
    if (rc != SQLITE_OK)
      goto dberr;
    rc = sqlite3_bind_int(ins, 2, *pEtype);
    if (rc != SQLITE_OK)
      goto dberr;
    sqlite3_step(ins);    
    rc = sqlite3_reset(ins);
    if (rc != SQLITE_OK)
      goto dberr;
  }
  rc = sqlite3_finalize(ins);
  ins=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
    
  
  do_send(sess->ssl, RESP_OK, NULL);
  dbaction=1;
  goto freeall;
 dberr:
  prtmsg("database error: %s", sqlite3_errmsg(sess->dbh));
 dberrnomsg:
  send_error(sess, ERR_OTHER, "Server internal error (database failure)");
  goto freeall;
 interr:
  send_error(sess, ERR_OTHER, "Server internal error");
  goto freeall;
 memerr:
  send_error(sess, ERR_OTHER, "Server internal error (out of memory)");
  goto freeall;
 badpkt:
  send_error(sess, ERR_BADREQ, "Packet was corrupt or too short");
 freeall:
  if (ins)
    sqlite3_finalize(ins);
  if (dbaction > 0)
    sql_commit_trans(sess);
  else if (dbaction < 0)
    sql_rollback_trans(sess);
  if (kadm_handle) {
    kadm5_free_principal_ent(kadm_handle, &ke);
    kadm5_destroy(kadm_handle);
  }
  if (realm) {
#if defined(HAVE_KRB5_REALM)
    krb5_xfree(realm);
#else
    krb5_free_default_realm(sess->kctx, realm);
#endif
  }

  if (target)
    krb5_free_principal(sess->kctx, target);
  free(principal);
  if (hostnames) {
    for (i=0; i < n; i++) {
      free(hostnames[i]);
    }
    free(hostnames);
  }
}

static void s_status(struct rekey_session *sess, mb_t buf)
{
  sqlite3_stmt *st=NULL;
  char *principal = NULL;
  const char *hostname=NULL;
  unsigned int f, l, n;
  int rc;
  size_t curlen;

  if (sess->is_admin == 0) {
    send_error(sess, ERR_AUTHZ, "Not authorized (you must be an administrator)");
    return;
  }

  if (buf_getint(buf, &l))
    goto badpkt;
  principal = malloc(l + 1);
  if (!principal)
    goto memerr;
  if (buf_getdata(buf, principal, l))
    goto badpkt;
  principal[l]=0;

  if (sql_init(sess))
    goto dberrnomsg;

  rc = sqlite3_prepare_v2(sess->dbh, 
                          "SELECT hostname,complete,attempted FROM principals,acl WHERE name=? AND principal = id",
                          -1, &st, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(st, 1, principal, strlen(principal), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  n=0;
  curlen=8;
  
  while (SQLITE_ROW == sqlite3_step(st)) {
    hostname = (const char *)sqlite3_column_text(st, 0);
    l = sqlite3_column_bytes(st, 0);
    if (hostname == NULL || l == 0)
      goto interr;
    if (!strcmp(hostname, "0"))
      goto dberr;
    f = 0;
    if (sqlite3_column_int(st, 1))
      f=STATUSFLAG_COMPLETE;
    if (sqlite3_column_int(st, 2))
      f=STATUSFLAG_ATTEMPTED;
    if (buf_setlength(buf, curlen + 4 + 4 + l))
      goto memerr;
    set_cursor(buf, curlen);
    if (buf_putint(buf, f) ||
        buf_putint(buf, l) ||
        buf_putdata(buf, hostname, l))
      goto interr;
    n++;
    curlen = curlen + 4 + 4 + l;
  }
  
  rc = sqlite3_finalize(st);
  st=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  if (n == 0) {
    send_error(sess, ERR_NOTFOUND, "Requested principal does not have rekey in progress");
  } else {
    reset_cursor(buf);
    buf_putint(buf, 0);
    buf_putint(buf, n);
    do_send(sess->ssl, RESP_STATUS, buf);
  }
  goto freeall;
 dberr:
  prtmsg("database error: %s", sqlite3_errmsg(sess->dbh));
 dberrnomsg:
  send_error(sess, ERR_OTHER, "Server internal error (database failure)");
  goto freeall;
 interr:
  send_error(sess, ERR_OTHER, "Server internal error");
  goto freeall;
 memerr:
  send_error(sess, ERR_OTHER, "Server internal error (out of memory)");
  goto freeall;
 badpkt:
  send_error(sess, ERR_BADREQ, "Packet was corrupt or too short");
 freeall:
  if (st)
    sqlite3_finalize(st);
  free(principal);
}

static void s_getkeys(struct rekey_session *sess, mb_t buf)
{
  int m, n, rc;
  size_t l, curlen, last;
  sqlite3_stmt *st, *st2, *updatt, *updcount;
  sqlite_int64 principal;
  const char *pname;
  int kvno, enctype, dbaction=0;
  const unsigned char *key;
  
  if (sess->is_host == 0) {
    send_error(sess, ERR_NOKEYS, "only hosts can fetch keys with this interface");
    return;
  } 
  if (sql_init(sess))
    goto dberrnomsg;

  if (sql_begin_trans(sess))
    goto dberrnomsg;
  dbaction=-1;
  
  rc = sqlite3_prepare(sess->dbh,"SELECT id, name, kvno FROM principals, acl WHERE acl.hostname=? AND acl.principal=principals.id",
                       -1, &st, NULL);

  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(st, 1, sess->hostname, 
                         strlen(sess->hostname), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_prepare(sess->dbh,"SELECT enctype, key from keys where principal=?",
		       -1, &st2, NULL);
  if (rc != SQLITE_OK)
    goto dberr;

  rc = sqlite3_prepare_v2(sess->dbh, 
			  "UPDATE acl SET attempted = 1 WHERE principal = ? AND hostname = ?;",
			  -1, &updatt, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(updatt, 2, sess->hostname, 
                         strlen(sess->hostname), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;

  rc = sqlite3_prepare_v2(sess->dbh, 
			  "UPDATE principal SET downloadcount = downloadcount +1 WHERE id = ?;",
			  -1, &updcount, NULL);
  if (rc != SQLITE_OK)
    goto dberr;

  m=0;
  curlen=4;
  
  while (SQLITE_ROW == sqlite3_step(st)) {
    principal=sqlite3_column_int64(st, 0);
    pname = (const char *)sqlite3_column_text(st, 1);
    kvno = sqlite3_column_int(st, 2);
    l = sqlite3_column_bytes(st, 1);
    if (pname == NULL || l == 0)
      goto interr;
    if (!strncmp(pname, "0", 2) || principal == 0)
      goto dberr;
    if (buf_setlength(buf, curlen + 8 + l)) /* name length, kvno */
      goto memerr;
    set_cursor(buf, curlen);
    if (buf_putint(buf, l) || buf_putdata(buf, pname, l) ||
	buf_putint(buf, kvno))
      goto interr;
    curlen=curlen + 8 + l;
    last = curlen; /* key count goes here */
    curlen += 4; /* key count */
    rc = sqlite3_bind_int64(st2, 1, principal);
    if (rc != SQLITE_OK)
      goto dberr;
    n=0;
    while (SQLITE_ROW == sqlite3_step(st2)) {
      enctype = sqlite3_column_int(st2, 0);
      key = sqlite3_column_blob(st2, 1);
      l = sqlite3_column_bytes(st2, 1);
      if (key == NULL || l == 0)
	goto interr;
      if (enctype == 0)
	goto dberr;
      if (buf_setlength(buf, curlen + 8 + l)) /* enctype, key length */
	goto memerr;
      set_cursor(buf, curlen);
      if (buf_putint(buf, enctype) || buf_putint(buf, l) ||
	  buf_putdata(buf, key, l))
	goto interr;
      curlen = curlen + 8 + l;
      
      n++;
    }
    set_cursor(buf, last);
    if (buf_putint(buf, n))
      goto interr;
    rc = sqlite3_reset(st2);
    if (rc != SQLITE_OK)
      goto dberr;
    if (n == 0)   
      goto interr;
    m++;

    rc = sqlite3_bind_int64(updatt, 1, principal);
    if (rc != SQLITE_OK)
      goto dberr;
    sqlite3_step(updatt);    
    rc = sqlite3_reset(updatt);
    if (rc != SQLITE_OK)
      goto dberr;

    rc = sqlite3_bind_int64(updcount, 1, principal);
    if (rc != SQLITE_OK)
      goto dberr;
    sqlite3_step(updcount);    
    rc = sqlite3_reset(updcount);
    if (rc != SQLITE_OK)
      goto dberr;
  }
  if (m == 0) {
    send_error(sess, ERR_NOKEYS, "No keys available for this host");
  } else {
    set_cursor(buf, 0);
    if (buf_putint(buf, m))
      goto interr;
    do_send(sess->ssl, RESP_KEYS, buf);
    dbaction=1;
  }    
  
  goto freeall;
 dberr:
  prtmsg("database error: %s", sqlite3_errmsg(sess->dbh));
 dberrnomsg:
  send_error(sess, ERR_OTHER, "Server internal error (database failure)");
  goto freeall;
 interr:
  send_error(sess, ERR_OTHER, "Server internal error");
  goto freeall;
 memerr:
  send_error(sess, ERR_OTHER, "Server internal error (out of memory)");
 freeall:
  if (st)
    sqlite3_finalize(st);
  if (st2)
    sqlite3_finalize(st2);
  if (updatt)
    sqlite3_finalize(updatt);
  if (updcount)
    sqlite3_finalize(updcount);
  if (dbaction > 0)
    sql_commit_trans(sess);
  else if (dbaction < 0)
    sql_rollback_trans(sess);
}

#ifdef HAVE_KADM5_CHPASS_PRINCIPAL_WITH_KEY
static int prepare_kadm_key(krb5_key_data *k, int kvno, int enctype, int keylen,
		   const unsigned char *keydata) {
  k->key_data_ver = 1;
  k->key_data_kvno = kvno;
  k->key_data_type[0]=enctype;
  k->key_data_length[0]=keylen;
  k->key_data_contents[0]=malloc(keylen);
  if (k->key_data_contents[0] == NULL)
    return 1;
  memcpy(k->key_data_contents[0], keydata, keylen);
  return 0;
}
#else
static int prepare_kadm_key(krb5_keyblock *k, int kvno, int enctype, int keylen,
		   const unsigned char *keydata) {
  Z_enctype(k)=enctype;
  Z_keylen(k)=keylen;
  Z_keydata(k)=malloc(keylen);
  if (Z_keydata(k) == NULL)
    return 1;
  memcpy(Z_keydata(k), keydata, keylen);
  return 0;
}
#endif    

static void s_commitkey(struct rekey_session *sess, mb_t buf)
{
  sqlite3_stmt *getprinc=NULL, *updcomp=NULL, *updcount=NULL;
  sqlite3_stmt *checkcomp=NULL, *updmsg=NULL, *selkey=NULL;
  sqlite3_stmt *del=NULL;
  sqlite_int64 princid;
  unsigned int l, kvno, nk=0, match, no_send = 0, enctype, keylen, i;
  char *principal = NULL;
  int dbaction=0, rc;
  char *realm=NULL;
  kadm5_config_params kadm_param;
  void *kadm_handle;
  krb5_principal target=NULL;
  kadm5_principal_ent_rec ke;
#ifdef HAVE_KADM5_CHPASS_PRINCIPAL_WITH_KEY
  krb5_key_data *k=NULL, *newk;
  int ksz = sizeof(krb5_key_data);
#else
  krb5_keyblock *k=NULL, *newk;
  int ksz = sizeof(krb5_keyblock);
#endif
  const unsigned char *keydata;
    

  if (sess->is_admin) {
    send_error(sess, ERR_BADOP, "Not implemented yet");
    return;
  } else if (sess->is_host == 0) {
    send_error(sess, ERR_AUTHZ, "Not authorized");
    return;
  }
  if (buf_getint(buf, &l))
    goto badpkt;
  principal = malloc(l + 1);
  if (!principal)
    goto memerr;
  if (buf_getdata(buf, principal, l))
    goto badpkt;
  principal[l]=0;
  rc = krb5_parse_name(sess->kctx, principal, &target);
  if (rc) {
    prtmsg("Cannot parse target name %s (kerberos error %s)", principal, krb5_get_err_text(sess->kctx, rc));
    send_error(sess, ERR_BADREQ, "Bad principal name");
    goto freeall;
  }

  if (buf_getint(buf, &kvno))
    goto badpkt;

  if (sql_init(sess))
    goto dberrnomsg;

  rc = sqlite3_prepare_v2(sess->dbh, 
                          "SELECT id from principals where name=? and kvno = ?",
                          -1, &getprinc, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_text(getprinc, 1, principal, strlen(principal), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int(getprinc, 2, kvno);
  if (rc != SQLITE_OK)
    goto dberr;
  match=0;
  princid = -1;
  while (SQLITE_ROW == sqlite3_step(getprinc)) {
    princid = sqlite3_column_int64(getprinc, 0);
    if (princid == 0)
      goto dberr;
    match++;
  }
  rc = sqlite3_finalize(getprinc);
  getprinc=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  if (match == 0) {
    send_error(sess, ERR_AUTHZ, "No rekey for this principal is in progress");
    prtmsg("%s tried to commit %s %d, but it is not active",
	   sess->hostname, principal, kvno);
    goto freeall;
  }

  if (sql_begin_trans(sess))
    goto dberr;
  dbaction = -1;

  rc = sqlite3_prepare_v2(sess->dbh, 
			  "UPDATE acl SET completed = 1 WHERE principal = ? AND hostname = ?;",
			  -1, &updcomp, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int64(updcomp, 1, princid);
  rc = sqlite3_bind_text(updcomp, 2, sess->hostname, 
                         strlen(sess->hostname), SQLITE_STATIC);
  if (rc != SQLITE_OK)
    goto dberr;
  sqlite3_step(updcomp);
  rc = sqlite3_finalize(updcomp);
  updcomp=NULL;
  if (rc != SQLITE_OK)
    goto dberr;

  rc = sqlite3_prepare_v2(sess->dbh, 
			  "UPDATE principal SET commitcount = commitcount +1 WHERE id = ?;",
			  -1, &updcount, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int64(updcount, 1, princid);
  if (rc != SQLITE_OK)
    goto dberr;
  sqlite3_step(updcount);
  rc = sqlite3_finalize(updcount);
  updcount=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  dbaction=0;
  if (sql_commit_trans(sess))
    goto dberr;
  /* at this point, the client doesn't care about future errors */
  do_send(sess->ssl, RESP_OK, NULL);
  no_send = 1;

  rc = sqlite3_prepare_v2(sess->dbh,
			  "SELECT principal FROM acl WHERE principal = ? AND complete = 0;",
			  -1, &checkcomp, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int64(checkcomp, 0, princid);
  if (rc != SQLITE_OK)
    goto dberr;
  match=0;
  while (SQLITE_ROW == sqlite3_step(checkcomp)) {
    match++;
  }
  rc = sqlite3_finalize(checkcomp);
  checkcomp=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  /* not done yet */
  if (match) {
    goto freeall;
  }
  
  rc = sqlite3_prepare_v2(sess->dbh, 
			  "UPDATE principal SET message = ? WHERE id = ?;",
			  -1, &updmsg, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int64(updcount, 2, princid);
  if (rc != SQLITE_OK)
    goto dberr;
  
  rc=krb5_get_default_realm(sess->kctx, &realm);
  if (rc) {
    prtmsg("Unable to get default realm: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }
  kadm_param.mask = KADM5_CONFIG_REALM;
  kadm_param.realm = realm;
  memset(&ke, 0, sizeof(ke));
#ifdef HAVE_KADM5_INIT_WITH_SKEY_CTX
  rc = kadm5_init_with_skey_ctx(sess->kctx, 
			    "rekey/admin", NULL, KADM5_ADMIN_SERVICE,
			    &kadm_param, KADM5_STRUCT_VERSION, 
			    KADM5_API_VERSION_2, &kadm_handle);
#else
  rc = kadm5_init_with_skey("rekey/admin", NULL, KADM5_ADMIN_SERVICE,
			    &kadm_param, KADM5_STRUCT_VERSION, 
			    KADM5_API_VERSION_2, NULL, &kadm_handle);
#endif
  if (rc) {
    prtmsg("Unable to initialize kadm5 library: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }

  rc = kadm5_get_principal(kadm_handle, target, &ke, KADM5_KVNO);
  if (rc) {
    if (rc == KADM5_UNK_PRINC) {
      prtmsg("Principal %s disappeared from kdc", principal);
      rc = sqlite3_bind_text(updmsg, 2, "Principal disappeared from kdc", 
			     strlen("Principal disappeared from kdc"), 
			     SQLITE_STATIC);
      if (rc != SQLITE_OK) {
	sqlite3_step(updmsg); /* finalize in freeall */
      }
      goto freeall;
    }
    prtmsg("Unable to initialize kadm5 library: %s", krb5_get_err_text(sess->kctx, rc));
    goto interr;
  }

  if (kvno != ke.kvno + 1) {
    prtmsg("kvno of %s changed from %d to %d; not finalizing commit", principal, kvno - 1, ke.kvno);
    rc = sqlite3_bind_text(updmsg, 2, "kvno changed on kdc", 
			   strlen("kvno changed on kdc"), 
			   SQLITE_STATIC);
    if (rc != SQLITE_OK) {
      sqlite3_step(updmsg); /* finalize in freeall */
    }
    goto freeall;
  }
  
  rc = sqlite3_prepare_v2(sess->dbh,
			  "SELECT enctype, key FROM keys WHERE principal = ?;",
			  -1, &selkey, NULL);
  if (rc != SQLITE_OK)
    goto dberr;
  rc = sqlite3_bind_int64(selkey, 0, princid);
  if (rc != SQLITE_OK)
    goto dberr;
  while (SQLITE_ROW == sqlite3_step(selkey)) {
    enctype = sqlite3_column_int(selkey, 0);
    keydata = sqlite3_column_blob(selkey, 1);
    keylen = sqlite3_column_bytes(selkey, 1);
    if (keydata == NULL || keylen == 0)
      goto interr;
    if (enctype == 0)
      goto dberr;
    if (enctype == ENCTYPE_DES_CBC_CRC)
      newk = realloc(k, ksz * (nk+3));
    else
      newk = realloc(k, ksz * (nk+1));
    if (newk == NULL)
      goto memerr;
    k = newk;

    if (prepare_kadm_key(&k[nk++], kvno, enctype, keylen, keydata))
      goto memerr;
    if (enctype == ENCTYPE_DES_CBC_CRC) {
      if (prepare_kadm_key(&k[nk++], kvno, ENCTYPE_DES_CBC_MD4, keylen, keydata))
	goto memerr;
      if (prepare_kadm_key(&k[nk++], kvno, ENCTYPE_DES_CBC_MD5, keylen, keydata))
	goto memerr;
    }
  }
  rc = sqlite3_finalize(selkey);
  selkey=NULL;
  if (rc != SQLITE_OK)
    goto dberr;
  if (nk == 0) {
    prtmsg("No keys found for %s; cannot commit", principal);
    goto interr;
  }
#ifdef HAVE_KADM5_CHPASS_PRINCIPAL_WITH_KEY
  rc = kadm5_chpass_principal_with_key(kadm_handle, target, nk, k);
#else
  rc = kadm5_setkey_principal(kadm_handle, target, k, nk);
#endif
  if (rc) {
    prtmsg("finalizing %s failed to update kdc: %s", 
	   krb5_get_err_text(sess->kctx, rc));
    
    rc = sqlite3_bind_text(updmsg, 2, "updating kdc failed", 
			   strlen("updating kdc failed"), 
			   SQLITE_STATIC);
    if (rc != SQLITE_OK) {
      sqlite3_step(updmsg); /* finalize in freeall */
    }
    goto freeall;
  }
  if (sql_begin_trans(sess))
    goto dberr;
  dbaction=-1;
  rc = sqlite3_prepare_v2(sess->dbh, 
			  "DELETE FROM keys WHERE principal = ?;",
			  -1, &del, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(del, 0, princid);
    if (rc == SQLITE_OK)
      sqlite3_step(del);
    rc = sqlite3_finalize(del);
    del=0;
  }
  if (rc == SQLITE_OK)
    rc = sqlite3_prepare_v2(sess->dbh, 
			    "DELETE FROM acl WHERE principal = ?;",
			    -1, &del, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(del, 0, princid);
    if (rc == SQLITE_OK)
      sqlite3_step(del);
    rc = sqlite3_finalize(del);
    del=0;
  }
  if (rc == SQLITE_OK)
    rc = sqlite3_prepare_v2(sess->dbh, 
			    "DELETE FROM principals WHERE id = ?;",
			    -1, &del, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(del, 0, princid);
    if (rc == SQLITE_OK)
      sqlite3_step(del);
    rc = sqlite3_finalize(del);
    del=0;
  }
  if (rc != SQLITE_OK)
    goto dberr;
  dbaction=1;

  goto freeall;
 dberr:
  prtmsg("database error: %s", sqlite3_errmsg(sess->dbh));
 dberrnomsg:
  if (no_send == 0)
    send_error(sess, ERR_OTHER, "Server internal error (database failure)");
  goto freeall;
 interr:
  if (no_send == 0)
    send_error(sess, ERR_OTHER, "Server internal error");
  goto freeall;
 memerr:
  if (no_send == 0)
    send_error(sess, ERR_OTHER, "Server internal error (out of memory)");
  goto freeall;
 badpkt:
  send_error(sess, ERR_BADREQ, "Packet was corrupt or too short");
 freeall:
  if (getprinc)
    sqlite3_finalize(getprinc);
  if (updcomp)
    sqlite3_finalize(updcomp);
  if (updcount)
    sqlite3_finalize(updcount);
  if (updmsg)
    sqlite3_finalize(updmsg);
  if (selkey)
    sqlite3_finalize(selkey);
  if (dbaction > 0)
    sql_commit_trans(sess);
  else if (dbaction < 0)
    sql_rollback_trans(sess);
  if (k) {
    for (i=0; i<nk; i++) {
#ifdef HAVE_KADM5_CHPASS_PRINCIPAL_WITH_KEY
      free(k[nk].key_data_contents[0]);
#else
      free(Z_keydata(&k[nk]));
#endif
    }
  }
  if (realm) {
#if defined(HAVE_KRB5_REALM)
    krb5_xfree(realm);
#else
    krb5_free_default_realm(sess->kctx, realm);
#endif
  }
  if (target)
    krb5_free_principal(sess->kctx, target);  
  free(principal);

}
static void s_simplekey(struct rekey_session *sess, mb_t buf)
{
  if (sess->is_admin == 0) {
    send_error(sess, ERR_AUTHZ, "Not authorized (you must be an administrator)");
    return;
  }
  send_error(sess, ERR_BADOP, "Not implemented yet");
}
static void s_abortreq(struct rekey_session *sess, mb_t buf)
{
  if (sess->is_admin == 0) {
    send_error(sess, ERR_AUTHZ, "Not authorized (you must be an administrator)");
    return;
  }
  send_error(sess, ERR_BADOP, "Not implemented yet");
}

static void (*func_table[])(struct rekey_session *, mb_t) = {
  NULL,
  s_auth,
  s_autherr,
  s_authchan,
  s_newreq,
  s_status,
  s_getkeys,
  s_commitkey,
  s_simplekey,
  s_abortreq
};

void run_session(int s) {
  struct rekey_session sess;
  mb_t buf;
  int opcode;

  memset(&sess, 0, sizeof(sess));
  buf = buf_alloc(1);
  if (!buf) {
    close(s);
    fatal("Cannot allocate memory: %s", strerror(errno));
  }
  sess.ssl = do_ssl_accept(s);
  child_cleanup();
  sess.initialized=1;
  for (;;) {
    opcode = do_recv(sess.ssl, buf);
    
    if (opcode == -1) {
      do_finalize(&sess);
      ssl_cleanup();
      fatal("Connection closed");
    }
    if (sess.authstate != 2 && opcode > 3) {
      send_error(&sess, ERR_AUTHZ, "Operation not allowed on unauthenticated connection");
      continue;
    }
    
    if (opcode <= 0 || opcode > MAX_OPCODE) {
       send_error(&sess, ERR_BADOP, "Function code was out of range");
    } else {
      func_table[opcode](&sess, buf);
      if (sess.initialized == 0)
        fatal("session terminated during operation %d, but handler did not exit", opcode);
    }
  }
}