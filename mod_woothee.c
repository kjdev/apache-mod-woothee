/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mod_woothee.c: Add/append HTTP request headers by Woothee
 *
 * The RequestHeaderForWoothee directive can be used to add
 * HTTP headers before a request message is processed.
 * Valid in both per-server and per-dir configurations.
 *
 * Syntax is:
 *
 *   RequestHeaderForWoothee action header item
 *
 * Where action is one of:
 *     set    - set this header, replacing any old value
 *     add    - add this header, possible resulting in two or more
 *              headers with the same name
 *     append - append this text onto any existing header of this same
 *     merge  - merge this text onto any existing header of this same,
 *              avoiding duplicate values
 */

#include "apr.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_buckets.h"

#include "apr_hash.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"

#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_expr.h"

#include "mod_ssl.h" /* for the ssl_var_lookup optional function defn */

#include "woothee.h"

typedef enum {
  hdr_add = 'a',              /* add header (could mean multiple hdrs) */
  hdr_set = 's',              /* set (replace old value) */
  hdr_append = 'm',           /* append (merge into any old value) */
  hdr_merge = 'g',            /* merge (merge, but avoid duplicates) */
  hdr_setifempty = 'i',       /* set value if header not already present*/
  hdr_note = 'n'              /* set value of header in a note */
} hdr_actions;

/*
 * magic cmd->info values
 */
static char hdr_in  = '0';

/* 'Magic' condition_var value to run action in post_read_request */
static const char* condition_early = "early";
/*
 * There is one "header_entry" per Header/RequestHeader config directive
 */
typedef struct {
  hdr_actions action;
  const char *header;
  const char *item;
  const char *condition_var;
  const char *subs;
  ap_expr_info_t *expr;
} header_entry;

/*
 * woothee_conf is our per-module configuration. This is used as both
 * a per-dir and per-server config
 */
typedef struct {
  apr_array_header_t *fixup_in;
} woothee_conf;

module AP_MODULE_DECLARE_DATA woothee_module;

/* Pointer to ssl_var_lookup, if available. */
static APR_OPTIONAL_FN_TYPE(ssl_var_lookup) *header_ssl_lookup = NULL;


/*
 * Config routines
 */

static void *
create_woothee_dir_config(apr_pool_t *p, char *d)
{
  woothee_conf *conf = apr_pcalloc(p, sizeof(*conf));

  conf->fixup_in = apr_array_make(p, 2, sizeof(header_entry));

  return conf;
}

static void *
merge_woothee_config(apr_pool_t *p, void *basev, void *overridesv)
{
  woothee_conf *newconf = apr_pcalloc(p, sizeof(*newconf));
  woothee_conf *base = basev;
  woothee_conf *overrides = overridesv;

  newconf->fixup_in = apr_array_append(p, base->fixup_in,
                                       overrides->fixup_in);

  return newconf;
}

/* handle RequestHeader and Header directive */
static APR_INLINE const char *
header_inout_cmd(cmd_parms *cmd, void *indirconf,
                 const char *action, const char *hdr,
                 const char *value, const char *subs, const char *envclause)
{
  woothee_conf *dirconf = indirconf;
  const char *condition_var = NULL;
  const char *colon;
  header_entry *new;
  ap_expr_info_t *expr = NULL;

  apr_array_header_t *fixup = dirconf->fixup_in;

  new = (header_entry *) apr_array_push(fixup);

  if (!strcasecmp(action, "set")) {
    new->action = hdr_set;
  } else if (!strcasecmp(action, "setifempty")) {
    new->action = hdr_setifempty;
  } else if (!strcasecmp(action, "add")) {
    new->action = hdr_add;
  } else if (!strcasecmp(action, "append")) {
    new->action = hdr_append;
  } else if (!strcasecmp(action, "merge")) {
    new->action = hdr_merge;
  } else if (!strcasecmp(action, "note")) {
    new->action = hdr_note;
  } else {
    return "first argument must be 'add', 'set', 'setifempty', 'append', "
      "'merge', 'note'.";
  }

  /* there's no subs, so envclause is really that argument */
  if (envclause != NULL) {
    return "Too many arguments to directive";
  }
  envclause = subs;

  if (!value) {
    return "Header requires three arguments";
  }

  /* Handle the envclause on Header */
  if (envclause != NULL) {
    if (strcasecmp(envclause, "early") == 0) {
      condition_var = condition_early;
    } else if (strncasecmp(envclause, "env=", 4) == 0) {
      if ((envclause[4] == '\0')
          || ((envclause[4] == '!') && (envclause[5] == '\0'))) {
        return "error: missing environment variable name. "
          "envclause should be in the form env=envar ";
      }
      condition_var = envclause + 4;
    } else if (strncasecmp(envclause, "expr=", 5) == 0) {
      const char *err = NULL;
      expr = ap_expr_parse_cmd(cmd, envclause + 5, 0, &err, NULL);
      if (err) {
        return apr_pstrcat(cmd->pool,
                           "Can't parse envclause/expression: ", err, NULL);
      }
    }
    else {
      return apr_pstrcat(cmd->pool, "Unknown parameter: ", envclause, NULL);
    }
  }

  if ((colon = ap_strchr_c(hdr, ':'))) {
    hdr = apr_pstrmemdup(cmd->pool, hdr, colon-hdr);
  }

  new->header = hdr;
  new->condition_var = condition_var;
  new->expr = expr;

  new->item = apr_pstrcat(cmd->pool, value, NULL);

  return NULL;
}

/* Handle directives */
static const char *
header_cmd(cmd_parms *cmd, void *indirconf, const char *args)
{
  const char *action;
  const char *hdr;
  const char *val;
  const char *envclause;
  const char *subs;

  action = ap_getword_conf(cmd->temp_pool, &args);
  hdr = ap_getword_conf(cmd->pool, &args);
  val = *args ? ap_getword_conf(cmd->pool, &args) : NULL;
  subs = *args ? ap_getword_conf(cmd->pool, &args) : NULL;
  envclause = *args ? ap_getword_conf(cmd->pool, &args) : NULL;

  if (*args) {
    return apr_pstrcat(cmd->pool, cmd->cmd->name,
                       " has too many arguments", NULL);
  }

  return header_inout_cmd(cmd, indirconf, action, hdr, val, subs, envclause);
}

/*
 * Process the item in the woothee struct.
 */
static char *
woothee_process_item(header_entry *hdr, request_rec *r, woothee_t *woothee)
{
  const char *v = NULL;
  const char *item = NULL;
  char *str = NULL;

  if (!woothee) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(02557)
                  "Can't evaluate value woothee");
    return "";
  }

  item = (const char *)hdr->item;

  if (item) {
    if (strcmp(item, "name") == 0) {
      v = woothee->name;
    } else if (strcmp(item, "os") == 0) {
      v = woothee->os;
    } else if (strcmp(item, "category") == 0) {
      v = woothee->category;
    } else if (strcmp(item, "os_version") == 0) {
      v = woothee->os_version;
    } else if (strcmp(item, "version") == 0) {
      v = woothee->version;
    } else if (strcmp(item, "vendor") == 0) {
      v = woothee->vendor;
    }
  }

  if (v) {
    str = apr_pstrdup(r->pool, v);
  }

  return str ? str : "";
}

static int
do_woothee_fixup(request_rec *r, apr_table_t *headers,
                 apr_array_header_t *fixup, int early)
{
  int i;
  const char *val;

  woothee_t *woothee = NULL;
  const char *ua = apr_table_get(headers, "User-Agent");
  if (ua != NULL) {
    woothee = woothee_parse(ua);
  }

  for (i = 0; i < fixup->nelts; ++i) {
    header_entry *hdr = &((header_entry *) (fixup->elts))[i];
    const char *envar = hdr->condition_var;

    /* ignore early headers in late calls */
    if (!early && (envar == condition_early)) {
      continue;
    }
    /* ignore late headers in early calls */
    else if (early && (envar != condition_early)) {
      continue;
    }
    /* Do we have an expression to evaluate? */
    else if (hdr->expr != NULL) {
      const char *err = NULL;
      int eval = ap_expr_exec(r, hdr->expr, &err);
      if (err) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01501)
                      "Failed to evaluate expression (%s) - ignoring", err);
      } else if (!eval) {
        continue;
      }
    }
    /* Have any conditional envar-controlled Header processing to do? */
    else if (envar && !early) {
      if (*envar != '!') {
        if (apr_table_get(r->subprocess_env, envar) == NULL) {
          continue;
        }
      } else {
        if (apr_table_get(r->subprocess_env, &envar[1]) != NULL) {
          continue;
        }
      }
    }

    switch (hdr->action) {
      case hdr_add:
        apr_table_addn(headers, hdr->header,
                       woothee_process_item(hdr, r, woothee));
        break;
      case hdr_append:
        apr_table_mergen(headers, hdr->header,
                         woothee_process_item(hdr, r, woothee));
        break;
      case hdr_merge:
        val = apr_table_get(headers, hdr->header);
        if (val == NULL) {
          apr_table_addn(headers, hdr->header,
                         woothee_process_item(hdr, r, woothee));
        } else {
          char *new_val = woothee_process_item(hdr, r, woothee);
          apr_size_t new_val_len = strlen(new_val);
          int tok_found = 0;

          /* modified version of logic in ap_get_token() */
          while (*val) {
            const char *tok_start;

            while (apr_isspace(*val)) {
              ++val;
            }
            tok_start = val;

            while (*val && *val != ',') {
              if (*val++ == '"') {
                while (*val) {
                  if (*val++ == '"') {
                    break;
                  }
                }
              }
            }

            if (new_val_len == (apr_size_t)(val - tok_start)
                && !strncmp(tok_start, new_val, new_val_len)) {
              tok_found = 1;
              break;
            }

            if (*val) {
              ++val;
            }
          }

          if (!tok_found) {
            apr_table_mergen(headers, hdr->header, new_val);
          }
        }
        break;
      case hdr_set:
        apr_table_setn(headers, hdr->header,
                       woothee_process_item(hdr, r, woothee));
        break;
      case hdr_setifempty:
        if (NULL == apr_table_get(headers, hdr->header)) {
          apr_table_setn(headers, hdr->header,
                         woothee_process_item(hdr, r, woothee));
        }
        break;
      case hdr_note:
        apr_table_setn(r->notes, woothee_process_item(hdr, r, woothee),
                       apr_table_get(headers, hdr->header));
        break;
    }
  }

  if (woothee) {
    woothee_delete(woothee);
  }

  return 1;
}

static apr_status_t
ap_woothee_fixup(request_rec *r)
{
  woothee_conf *dirconf = ap_get_module_config(r->per_dir_config,
                                               &woothee_module);

  /* do the fixup */
  if (dirconf->fixup_in->nelts) {
    do_woothee_fixup(r, r->headers_in, dirconf->fixup_in, 0);
  }

  return DECLINED;
}

static apr_status_t
ap_woothee_early(request_rec *r)
{
  woothee_conf *dirconf = ap_get_module_config(r->per_dir_config,
                                               &woothee_module);

  /* do the fixup */
  if (dirconf->fixup_in->nelts) {
    if (!do_woothee_fixup(r, r->headers_in, dirconf->fixup_in, 1)) {
      ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r, APLOGNO(01504)
                    "Regular expression replacement failed "
                    "(replacement too long?)");
      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }

  return DECLINED;
}

static const command_rec woothee_cmds[] =
{
  AP_INIT_RAW_ARGS("RequestHeaderForWoothee",
                   header_cmd, &hdr_in, OR_FILEINFO,
                   "an action, header and item followed by optional env "
                   "clause"),
  {NULL}
};

static int
header_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                   apr_pool_t *ptemp, server_rec *s)
{
  header_ssl_lookup = APR_RETRIEVE_OPTIONAL_FN(ssl_var_lookup);
  return OK;
}

static void
register_hooks(apr_pool_t *p)
{
  ap_hook_post_config(header_post_config,NULL,NULL,APR_HOOK_MIDDLE);
  ap_hook_fixups(ap_woothee_fixup, NULL, NULL, APR_HOOK_LAST);
  ap_hook_post_read_request(ap_woothee_early, NULL, NULL, APR_HOOK_FIRST);
}

AP_DECLARE_MODULE(woothee) =
{
  STANDARD20_MODULE_STUFF,
  create_woothee_dir_config,  /* dir config creater */
  merge_woothee_config,       /* dir merger --- default is to override */
  NULL,                       /* server config */
  NULL,                       /* merge server configs */
  woothee_cmds,               /* command apr_table_t */
  register_hooks              /* register hooks */
};
