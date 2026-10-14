/*
 * mod_sfv - SFV (Simple File Verification) checking for ProFTPD
 *
 * After every successful STOR, this module:
 *   - If the uploaded file is a .sfv: parses it, verifies any listed files
 *     already present, and (optionally) creates "<name>-missing" markers
 *     for listed files not yet uploaded.
 *   - Otherwise: looks for a .sfv in the same directory that lists the
 *     file, computes its CRC32, and compares. Good files have their
 *     "-missing" marker and any stale ".bad" copy removed; bad files are
 *     kept, renamed to *.bad, or deleted, and get a "-missing" marker.
 *   - Maintains a status directory in the release directory, e.g.
 *     "[INCOMPLETE 15 of 20F]" which becomes "[COMPLETE 20F]".
 *
 * After a successful DELE of a file listed in a .sfv, the marker is
 * recreated and the status directory updated.
 *
 * Configuration directives:
 *   SFVEngine          on|off                  (default off)
 *   SFVLog             /path/to/file|none      (server-level only)
 *   SFVBadFiles        keep|rename|delete      (default rename)
 *   SFVMissingMarkers  on|off                  (default on)
 *   SFVStatusDirs      on|off                  (default on)
 *   SFVIncompleteDir   "format"                (default "[INCOMPLETE %o of %tF]")
 *   SFVCompleteDir     "format"                (default "[COMPLETE %tF]")
 *
 * Status format variables: %o = files OK, %t = files listed,
 * %m = files missing/bad, %% = literal '%'. Formats must start with
 * literal text; existing directories whose names begin with that text
 * are treated as old status directories and removed (only if empty).
 *
 * All directives except SFVLog may be used in <Directory> and
 * <Anonymous> sections.
 *
 * Requires ProFTPD 1.3.4 or later (uses the mod_xfer.store-path note).
 */

#include "conf.h"
#include "privs.h"
#include <ctype.h>

#define MOD_SFV_VERSION "mod_sfv/0.2"

#if PROFTPD_VERSION_NUMBER < 0x0001030402
# error "ProFTPD 1.3.4rc2 or later required"
#endif

#define SFV_BAD_KEEP    0
#define SFV_BAD_RENAME  1
#define SFV_BAD_DELETE  2

#define SFV_MISSING_SUFFIX  "-missing"
#define SFV_BAD_SUFFIX      ".bad"

#define SFV_DEFAULT_INCOMPLETE  "[INCOMPLETE %o of %tF]"
#define SFV_DEFAULT_COMPLETE    "[COMPLETE %tF]"

module sfv_module;

static int sfv_logfd = -1;
static uint32_t sfv_crc_table[256];

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int sfv_get_int(const char *directive, int def) {
  int *v = get_param_ptr(CURRENT_CONF, directive, FALSE);
  return v ? *v : def;
}

static const char *sfv_get_str(const char *directive, const char *def) {
  const char *v = get_param_ptr(CURRENT_CONF, directive, FALSE);
  return v ? v : def;
}

static void sfv_crc32_init(void) {
  uint32_t i, j, c;

  for (i = 0; i < 256; i++) {
    c = i;
    for (j = 0; j < 8; j++) {
      c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    }
    sfv_crc_table[i] = c;
  }
}

/* Compute the CRC32 of a file. Returns 0 on success, -1 on error. */
static int sfv_crc32_file(const char *path, uint32_t *crc_out) {
  pr_fh_t *fh;
  unsigned char buf[16384];
  uint32_t crc = 0xFFFFFFFFU;
  int n, i;

  fh = pr_fsio_open(path, O_RDONLY);
  if (fh == NULL) {
    return -1;
  }

  while ((n = pr_fsio_read(fh, (char *) buf, sizeof(buf))) > 0) {
    pr_signals_handle();
    for (i = 0; i < n; i++) {
      crc = sfv_crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
  }

  pr_fsio_close(fh);

  if (n < 0) {
    return -1;
  }

  *crc_out = crc ^ 0xFFFFFFFFU;
  return 0;
}

static int sfv_has_suffix(const char *name, const char *suffix) {
  size_t nlen = strlen(name), slen = strlen(suffix);

  if (nlen <= slen) {
    return FALSE;
  }
  return strcasecmp(name + nlen - slen, suffix) == 0;
}

/* Parse one SFV line of the form "filename CRC32HEX". Comment lines start
 * with ';'. Filenames may contain spaces; the CRC is the last token.
 * Modifies the line in place. Returns 0 on a valid entry, -1 otherwise.
 */
static int sfv_parse_line(char *line, char **name, uint32_t *crc) {
  char *p, *end, *tok, *endp;
  size_t len;
  int i;

  len = strlen(line);
  while (len > 0 && isspace((int) line[len - 1])) {
    line[--len] = '\0';
  }

  p = line;
  while (*p && isspace((int) *p)) {
    p++;
  }

  if (*p == '\0' || *p == ';') {
    return -1;
  }

  /* Find the start of the last whitespace-separated token. */
  end = p + strlen(p);
  tok = end;
  while (tok > p && !isspace((int) *(tok - 1))) {
    tok--;
  }

  if (tok == p || strlen(tok) != 8) {
    return -1;
  }

  for (i = 0; i < 8; i++) {
    if (!isxdigit((int) tok[i])) {
      return -1;
    }
  }

  *crc = (uint32_t) strtoul(tok, &endp, 16);
  if (*endp != '\0') {
    return -1;
  }

  /* Terminate the filename and strip trailing whitespace. */
  end = tok;
  while (end > p && isspace((int) *(end - 1))) {
    end--;
  }
  *end = '\0';

  if (*p == '\0') {
    return -1;
  }

  *name = p;
  return 0;
}

/* Reject names that would escape the directory. */
static int sfv_name_is_safe(const char *name) {
  if (strchr(name, '/') != NULL ||
      strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0) {
    return FALSE;
  }
  return TRUE;
}

/* Search a single .sfv file for fname (case-insensitive).
 * Returns 1 if found (crc filled in), 0 if not, -1 on error.
 */
static int sfv_find_in(const char *sfv_path, const char *fname,
    uint32_t *crc) {
  pr_fh_t *fh;
  char line[1024];
  char *name;
  uint32_t want;

  fh = pr_fsio_open(sfv_path, O_RDONLY);
  if (fh == NULL) {
    return -1;
  }

  while (pr_fsio_gets(line, sizeof(line), fh) != NULL) {
    pr_signals_handle();

    if (sfv_parse_line(line, &name, &want) < 0) {
      continue;
    }

    if (strcasecmp(name, fname) == 0) {
      *crc = want;
      pr_fsio_close(fh);
      return 1;
    }
  }

  pr_fsio_close(fh);
  return 0;
}

/* Look through every .sfv in dir for fname.
 * Returns 1 if found, 0 if .sfv files exist but none list the file,
 * -1 if there is no .sfv in the directory.
 */
static int sfv_lookup(pool *p, const char *dir, const char *fname,
    uint32_t *crc, const char **sfv_used) {
  void *dirh;
  struct dirent *de;
  array_header *sfvs;
  unsigned int i;

  /* Collect names first, so we don't hold the directory open while
   * opening other files.
   */
  sfvs = make_array(p, 2, sizeof(char *));

  dirh = pr_fsio_opendir(dir);
  if (dirh == NULL) {
    return -1;
  }

  while ((de = pr_fsio_readdir(dirh)) != NULL) {
    if (sfv_has_suffix(de->d_name, ".sfv")) {
      *((char **) push_array(sfvs)) = pstrdup(p, de->d_name);
    }
  }

  pr_fsio_closedir(dirh);

  if (sfvs->nelts == 0) {
    return -1;
  }

  for (i = 0; i < sfvs->nelts; i++) {
    char *name = ((char **) sfvs->elts)[i];

    pr_signals_handle();

    if (sfv_find_in(pdircat(p, dir, name, NULL), fname, crc) == 1) {
      *sfv_used = name;
      return 1;
    }
  }

  return 0;
}

static void sfv_create_marker(pool *p, const char *path) {
  const char *marker = pstrcat(p, path, SFV_MISSING_SUFFIX, NULL);
  struct stat st;
  pr_fh_t *fh;

  if (!sfv_get_int("SFVMissingMarkers", TRUE)) {
    return;
  }

  if (pr_fsio_stat(marker, &st) == 0) {
    return;
  }

  fh = pr_fsio_open(marker, O_CREAT|O_WRONLY|O_TRUNC);
  if (fh != NULL) {
    pr_fsio_close(fh);
  }
}

static void sfv_remove_marker(pool *p, const char *path) {
  const char *marker = pstrcat(p, path, SFV_MISSING_SUFFIX, NULL);
  (void) pr_fsio_unlink(marker);
}

/* A good copy arrived: drop the marker and any old .bad copy. */
static void sfv_mark_good(pool *p, const char *path) {
  sfv_remove_marker(p, path);
  (void) pr_fsio_unlink(pstrcat(p, path, SFV_BAD_SUFFIX, NULL));
}

static const char *sfv_handle_bad(pool *p, const char *path, int action) {
  const char *result = NULL;

  switch (action) {
    case SFV_BAD_DELETE:
      if (pr_fsio_unlink(path) == 0) {
        result = "deleted";
      }
      break;

    case SFV_BAD_RENAME: {
      const char *bad = pstrcat(p, path, SFV_BAD_SUFFIX, NULL);
      if (pr_fsio_rename(path, bad) == 0) {
        result = "renamed to " SFV_BAD_SUFFIX;
      }
      break;
    }

    default:
      result = "kept";
      break;
  }

  if (result == NULL) {
    result = pstrcat(p, "action failed: ", strerror(errno), NULL);
  }

  /* The file is now absent or unverified either way. */
  sfv_create_marker(p, path);

  return result;
}

/* ---------------------------------------------------------------------
 * Status directories
 * ------------------------------------------------------------------- */

static const char *sfv_expand(pool *p, const char *fmt, unsigned int ok,
    unsigned int total) {
  char buf[256];
  size_t n = 0;
  const char *s;

  for (s = fmt; *s != '\0' && n < sizeof(buf) - 12; s++) {
    if (*s == '%' && s[1] != '\0') {
      s++;
      switch (*s) {
        case 'o':
          n += snprintf(buf + n, sizeof(buf) - n, "%u", ok);
          break;

        case 't':
          n += snprintf(buf + n, sizeof(buf) - n, "%u", total);
          break;

        case 'm':
          n += snprintf(buf + n, sizeof(buf) - n, "%u", total - ok);
          break;

        case '%':
          buf[n++] = '%';
          break;

        default:
          buf[n++] = '%';
          buf[n++] = *s;
          break;
      }

    } else {
      buf[n++] = *s;
    }
  }

  buf[n] = '\0';
  return pstrdup(p, buf);
}

static int sfv_has_prefix(const char *name, const char *prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

/* Recount the release in dir and replace its status directory. */
static void sfv_update_status(cmd_rec *cmd, const char *dir) {
  pool *p = cmd->tmp_pool;
  const char *inc_fmt, *comp_fmt, *inc_prefix, *comp_prefix, *name;
  array_header *sfvs, *olds;
  void *dirh;
  struct dirent *de;
  unsigned int i, total = 0, ok = 0;

  if (!sfv_get_int("SFVStatusDirs", TRUE)) {
    return;
  }

  inc_fmt = sfv_get_str("SFVIncompleteDir", SFV_DEFAULT_INCOMPLETE);
  comp_fmt = sfv_get_str("SFVCompleteDir", SFV_DEFAULT_COMPLETE);
  inc_prefix = pstrndup(p, inc_fmt, strcspn(inc_fmt, "%"));
  comp_prefix = pstrndup(p, comp_fmt, strcspn(comp_fmt, "%"));

  sfvs = make_array(p, 2, sizeof(char *));
  olds = make_array(p, 2, sizeof(char *));

  dirh = pr_fsio_opendir(dir);
  if (dirh == NULL) {
    return;
  }

  while ((de = pr_fsio_readdir(dirh)) != NULL) {
    if (sfv_has_suffix(de->d_name, ".sfv")) {
      *((char **) push_array(sfvs)) = pdircat(p, dir, de->d_name, NULL);

    } else if (sfv_has_prefix(de->d_name, inc_prefix) ||
               sfv_has_prefix(de->d_name, comp_prefix)) {
      *((char **) push_array(olds)) = pdircat(p, dir, de->d_name, NULL);
    }
  }

  pr_fsio_closedir(dirh);

  /* Remove old status dirs (rmdir only succeeds on empty dirs). */
  for (i = 0; i < olds->nelts; i++) {
    const char *old = ((char **) olds->elts)[i];
    struct stat st;

    if (pr_fsio_lstat(old, &st) == 0 && S_ISDIR(st.st_mode)) {
      (void) pr_fsio_rmdir(old);
    }
  }

  /* Count listed files that are present and have no marker. */
  for (i = 0; i < sfvs->nelts; i++) {
    pr_fh_t *fh;
    char line[1024];

    fh = pr_fsio_open(((char **) sfvs->elts)[i], O_RDONLY);
    if (fh == NULL) {
      continue;
    }

    while (pr_fsio_gets(line, sizeof(line), fh) != NULL) {
      char *fname;
      const char *fpath;
      uint32_t crc;
      struct stat st;

      pr_signals_handle();

      if (sfv_parse_line(line, &fname, &crc) < 0 ||
          !sfv_name_is_safe(fname)) {
        continue;
      }

      total++;
      fpath = pdircat(p, dir, fname, NULL);

      if (pr_fsio_stat(fpath, &st) == 0 &&
          S_ISREG(st.st_mode) &&
          pr_fsio_stat(pstrcat(p, fpath, SFV_MISSING_SUFFIX, NULL),
            &st) < 0) {
        ok++;
      }
    }

    pr_fsio_close(fh);
  }

  if (total == 0) {
    return;
  }

  name = sfv_expand(p, ok == total ? comp_fmt : inc_fmt, ok, total);

  if (pr_fsio_mkdir(pdircat(p, dir, name, NULL), 0755) < 0 &&
      errno != EEXIST) {
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "unable to create status dir '%s' in '%s': %s", name, dir,
      strerror(errno));
  }

  if (ok == total) {
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "%s: release complete (%u files)", dir, total);
  }
}

/* ---------------------------------------------------------------------
 * Upload processing
 * ------------------------------------------------------------------- */

/* A .sfv was uploaded: verify what's already there, mark what's missing. */
static void sfv_process_sfv(cmd_rec *cmd, const char *dir,
    const char *sfv_path) {
  pool *p = cmd->tmp_pool;
  pr_fh_t *fh;
  char line[1024];
  unsigned int total = 0, ok = 0, bad = 0, missing = 0;
  int action = sfv_get_int("SFVBadFiles", SFV_BAD_RENAME);

  fh = pr_fsio_open(sfv_path, O_RDONLY);
  if (fh == NULL) {
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "unable to open '%s': %s", sfv_path, strerror(errno));
    return;
  }

  while (pr_fsio_gets(line, sizeof(line), fh) != NULL) {
    char *name;
    const char *fpath, *result;
    uint32_t want, got;
    struct stat st;

    pr_signals_handle();

    if (sfv_parse_line(line, &name, &want) < 0 ||
        !sfv_name_is_safe(name)) {
      continue;
    }

    total++;
    fpath = pdircat(p, dir, name, NULL);

    if (pr_fsio_stat(fpath, &st) < 0) {
      missing++;
      sfv_create_marker(p, fpath);
      continue;
    }

    if (sfv_crc32_file(fpath, &got) < 0) {
      pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
        "unable to read '%s': %s", fpath, strerror(errno));
      continue;
    }

    if (got == want) {
      ok++;
      sfv_mark_good(p, fpath);

    } else {
      bad++;
      result = sfv_handle_bad(p, fpath, action);
      pr_response_add(R_DUP, "SFV: %s BAD (expected %08X, got %08X) - %s",
        name, (unsigned int) want, (unsigned int) got, result);
      pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
        "%s: BAD CRC (expected %08X, got %08X), %s", fpath,
        (unsigned int) want, (unsigned int) got, result);
    }
  }

  pr_fsio_close(fh);

  pr_response_add(R_DUP, "SFV: %u listed, %u OK, %u bad, %u missing",
    total, ok, bad, missing);
  pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
    "%s uploaded by %s: %u listed, %u OK, %u bad, %u missing", sfv_path,
    session.user ? session.user : "(unknown)", total, ok, bad, missing);
}

/* A regular file was uploaded: check it against the directory's .sfv. */
static void sfv_check_upload(cmd_rec *cmd, const char *dir,
    const char *fname, const char *path) {
  pool *p = cmd->tmp_pool;
  const char *sfv_used = NULL, *result;
  uint32_t want, got;
  int res;

  res = sfv_lookup(p, dir, fname, &want, &sfv_used);
  if (res < 0) {
    pr_trace_msg("sfv", 8, "no .sfv in '%s', skipping '%s'", dir, fname);
    return;
  }

  if (res == 0) {
    pr_trace_msg("sfv", 8, "'%s' not listed in any .sfv", fname);
    return;
  }

  if (sfv_crc32_file(path, &got) < 0) {
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "unable to read '%s': %s", path, strerror(errno));
    return;
  }

  if (got == want) {
    sfv_mark_good(p, path);
    pr_response_add(R_DUP, "SFV: CRC OK (%08X)", (unsigned int) got);
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "%s: CRC OK (%08X) per %s, user %s", path, (unsigned int) got,
      sfv_used, session.user ? session.user : "(unknown)");

  } else {
    result = sfv_handle_bad(p, path,
      sfv_get_int("SFVBadFiles", SFV_BAD_RENAME));
    pr_response_add(R_DUP, "SFV: CRC MISMATCH (expected %08X, got %08X) - %s",
      (unsigned int) want, (unsigned int) got, result);
    pr_log_writefile(sfv_logfd, MOD_SFV_VERSION,
      "%s: BAD CRC (expected %08X, got %08X) per %s, user %s, %s", path,
      (unsigned int) want, (unsigned int) got, sfv_used,
      session.user ? session.user : "(unknown)", result);
  }
}

/* Split path into a pool-allocated directory and a pointer to the base. */
static void sfv_split_path(pool *p, const char *path, char **dir,
    const char **base) {
  const char *slash = strrchr(path, '/');

  if (slash == NULL) {
    *dir = pstrdup(p, ".");
    *base = path;

  } else {
    *dir = (slash == path) ? pstrdup(p, "/") :
      pstrndup(p, path, slash - path);
    *base = slash + 1;
  }
}

static int sfv_is_own_file(const char *base) {
  return sfv_has_suffix(base, SFV_MISSING_SUFFIX) ||
    sfv_has_suffix(base, SFV_BAD_SUFFIX);
}

/* ---------------------------------------------------------------------
 * Command handlers
 * ------------------------------------------------------------------- */

MODRET sfv_post_stor(cmd_rec *cmd) {
  const char *path, *base;
  char *dir;

  if (!sfv_get_int("SFVEngine", FALSE)) {
    return PR_DECLINED(cmd);
  }

  path = pr_table_get(cmd->notes, "mod_xfer.store-path", NULL);
  if (path == NULL) {
    path = dir_best_path(cmd->tmp_pool, cmd->arg);
  }
  if (path == NULL) {
    return PR_DECLINED(cmd);
  }

  sfv_split_path(cmd->tmp_pool, path, &dir, &base);

  /* Don't treat our own marker/bad files as uploads. */
  if (sfv_is_own_file(base)) {
    return PR_DECLINED(cmd);
  }

  if (sfv_has_suffix(base, ".sfv")) {
    sfv_process_sfv(cmd, dir, path);

  } else {
    sfv_check_upload(cmd, dir, base, path);
  }

  sfv_update_status(cmd, dir);
  return PR_DECLINED(cmd);
}

MODRET sfv_post_dele(cmd_rec *cmd) {
  const char *path, *base, *sfv_used = NULL;
  char *dir;
  uint32_t crc;

  if (!sfv_get_int("SFVEngine", FALSE)) {
    return PR_DECLINED(cmd);
  }

  path = dir_best_path(cmd->tmp_pool, cmd->arg);
  if (path == NULL) {
    return PR_DECLINED(cmd);
  }

  sfv_split_path(cmd->tmp_pool, path, &dir, &base);

  if (!sfv_is_own_file(base) &&
      !sfv_has_suffix(base, ".sfv") &&
      sfv_lookup(cmd->tmp_pool, dir, base, &crc, &sfv_used) == 1) {
    sfv_create_marker(cmd->tmp_pool, path);
  }

  /* Also covers deleting the .sfv itself: no listed files, so the old
   * status dir is just removed.
   */
  sfv_update_status(cmd, dir);
  return PR_DECLINED(cmd);
}

/* ---------------------------------------------------------------------
 * Configuration handlers
 * ------------------------------------------------------------------- */

MODRET sfv_set_bool(cmd_rec *cmd) {
  int b;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);

  b = get_boolean(cmd, 1);
  if (b == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = b;
  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: SFVEngine on|off */
MODRET set_sfvengine(cmd_rec *cmd) {
  return sfv_set_bool(cmd);
}

/* usage: SFVMissingMarkers on|off */
MODRET set_sfvmissingmarkers(cmd_rec *cmd) {
  return sfv_set_bool(cmd);
}

/* usage: SFVStatusDirs on|off */
MODRET set_sfvstatusdirs(cmd_rec *cmd) {
  return sfv_set_bool(cmd);
}

/* usage: SFVIncompleteDir "format"
 *        SFVCompleteDir "format"
 */
MODRET set_sfvstatusformat(cmd_rec *cmd) {
  const char *fmt;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);

  fmt = cmd->argv[1];

  if (strchr(fmt, '/') != NULL) {
    CONF_ERROR(cmd, "format must not contain '/'");
  }

  if (strcspn(fmt, "%") == 0) {
    CONF_ERROR(cmd, "format must begin with literal text, e.g. \"[COMPLETE \"");
  }

  c = add_config_param_str(cmd->argv[0], 1, fmt);
  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: SFVBadFiles keep|rename|delete */
MODRET set_sfvbadfiles(cmd_rec *cmd) {
  int action;
  config_rec *c;
  const char *arg;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);

  arg = cmd->argv[1];
  if (strcasecmp(arg, "keep") == 0) {
    action = SFV_BAD_KEEP;

  } else if (strcasecmp(arg, "rename") == 0) {
    action = SFV_BAD_RENAME;

  } else if (strcasecmp(arg, "delete") == 0) {
    action = SFV_BAD_DELETE;

  } else {
    CONF_ERROR(cmd, "expected keep, rename, or delete");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = action;
  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: SFVLog path|none */
MODRET set_sfvlog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strcasecmp(cmd->argv[1], "none") != 0 &&
      pr_fs_valid_path(cmd->argv[1]) < 0) {
    CONF_ERROR(cmd, "must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* ---------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------- */

static int sfv_init(void) {
  sfv_crc32_init();
  return 0;
}

static int sfv_sess_init(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "SFVLog", FALSE);
  if (c != NULL) {
    const char *path = c->argv[0];

    if (strcasecmp(path, "none") != 0) {
      int res, xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(path, &sfv_logfd, PR_LOG_SYSTEM_MODE);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        pr_log_debug(DEBUG0, MOD_SFV_VERSION
          ": unable to open SFVLog '%s': %s", path,
          res == -1 ? strerror(xerrno) :
          res == -2 ? "parent directory is world-writable" :
          "path is a symlink");
        sfv_logfd = -1;
      }
    }
  }

  return 0;
}

/* ---------------------------------------------------------------------
 * Module tables
 * ------------------------------------------------------------------- */

static conftable sfv_conftab[] = {
  { "SFVEngine",         set_sfvengine,         NULL },
  { "SFVLog",            set_sfvlog,            NULL },
  { "SFVBadFiles",       set_sfvbadfiles,       NULL },
  { "SFVMissingMarkers", set_sfvmissingmarkers, NULL },
  { "SFVStatusDirs",     set_sfvstatusdirs,     NULL },
  { "SFVIncompleteDir",  set_sfvstatusformat,   NULL },
  { "SFVCompleteDir",    set_sfvstatusformat,   NULL },
  { NULL }
};

static cmdtable sfv_cmdtab[] = {
  { POST_CMD, C_STOR, G_NONE, sfv_post_stor, FALSE, FALSE },
  { POST_CMD, C_STOU, G_NONE, sfv_post_stor, FALSE, FALSE },
  { POST_CMD, C_DELE, G_NONE, sfv_post_dele, FALSE, FALSE },
  { 0, NULL }
};

module sfv_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "sfv",

  /* Configuration directive table */
  sfv_conftab,

  /* Command handler table */
  sfv_cmdtab,

  /* Authentication handler table */
  NULL,

  /* Module initialization */
  sfv_init,

  /* Session initialization */
  sfv_sess_init,

  /* Module version */
  MOD_SFV_VERSION
};