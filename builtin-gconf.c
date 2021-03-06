/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * NOTE: This module implements just enough of the GConf API to allow us
 * to detach mce from D-Bus session bus. By no means is this even meant
 * to be a complete GConf replacement.
 *
 * CAVEATS include *at least*:
 * - the keys have no hierarchy
 * - anything related to directories is simply ignored
 * - gconf_client_get() returns the real thing, not deep copied duplicate
 * - pair values are not supported
 * - adding new values is not supported
 * - value types can't be changed
 * ------------------------------------------------------------------------- */

#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <errno.h>

#include "mce-log.h"
#include "mce-io.h"

/* ========================================================================= *
 *
 * CONFIGURATION
 *
 * ========================================================================= */

/** Provide additional values for type testing purposes */
#define GCONF_ADD_DEBUG_VALUES 0

/** Enable error logging to stderr via gconf_log_error() */
#define GCONF_ENABLE_ERROR_LOGGING 01

/** Enable debug logging to stderr via gconf_log_debug() */
#define GCONF_ENABLE_DEBUG_LOGGING 01

/** Path to persistent storage file */
#define VALUES_PATH G_STRINGIFY(MCE_VAR_DIR)"/builtin-gconf.values"

/* ========================================================================= *
 *
 * MACROS
 *
 * ========================================================================= */

/** Tag unused parameters as such to avoid compilation warnings */
#define unused(variable) (void)&variable

/* ========================================================================= *
 *
 * TYPES
 *
 * ========================================================================= */

typedef enum
{
  GCONF_ERROR_SUCCESS              =  0,
  GCONF_ERROR_FAILED               =  1,

  GCONF_ERROR_NO_SERVER            =  2,
  GCONF_ERROR_NO_PERMISSION        =  3,
  GCONF_ERROR_BAD_ADDRESS          =  4,
  GCONF_ERROR_BAD_KEY              =  5,

  GCONF_ERROR_PARSE_ERROR          =  6,
  GCONF_ERROR_CORRUPT              =  7,
  GCONF_ERROR_TYPE_MISMATCH        =  8,
  GCONF_ERROR_IS_DIR               =  9,
  GCONF_ERROR_IS_KEY               = 10,
  GCONF_ERROR_OVERRIDDEN           = 11,
  GCONF_ERROR_OAF_ERROR            = 12,
  GCONF_ERROR_LOCAL_ENGINE         = 13,
  GCONF_ERROR_LOCK_FAILED          = 14,
  GCONF_ERROR_NO_WRITABLE_DATABASE = 15,
  GCONF_ERROR_IN_SHUTDOWN          = 16,
} GConfError;

typedef enum
{
  GCONF_VALUE_INVALID,
  GCONF_VALUE_STRING,
  GCONF_VALUE_INT,
  GCONF_VALUE_FLOAT,
  GCONF_VALUE_BOOL,
  GCONF_VALUE_SCHEMA,
  GCONF_VALUE_LIST,
  GCONF_VALUE_PAIR

} GConfValueType;

typedef struct GConfValue
{
  // public

  GConfValueType type;

  // private

  int refcount;

  union {
    gboolean b;
    gint     i;
    char    *s;
    double   f;
  } data;

  GConfValueType  list_type;
  GSList         *list_head;

} GConfValue;

typedef struct GConfEntry
{
  // public

  char *key;
  GConfValue *value;

  // private

  char *def;

} GConfEntry;

typedef struct GConfClient
{
  // public

  // (nothing)

  // private

  GSList  *entries;

  GSList  *notify_list;

} GConfClient;

typedef enum
{
  GCONF_CLIENT_PRELOAD_NONE,
  GCONF_CLIENT_PRELOAD_ONELEVEL,
  GCONF_CLIENT_PRELOAD_RECURSIVE
} GConfClientPreloadType;

typedef void (*GConfClientNotifyFunc)(GConfClient *client,
                                      guint cnxn_id,
                                      GConfEntry *entry,
                                      gpointer user_data);

typedef struct GConfClientNotify
{
  guint                 id;
  gchar                *namespace_section;
  GConfClientNotifyFunc func;
  gpointer              user_data;
  GFreeFunc             destroy_notify;

} GConfClientNotify;

/* ========================================================================= *
 *
 * FUNCTION PROTOTYPES
 *
 * ========================================================================= */

static GQuark gconf_error_quark(void);
static void gconf_set_error(GError **err, gint code, const gchar *fmt, ...);
static char *gconf_strip_string(char *str);
static int gconf_parse_int(const char *str);
static double gconf_parse_float(const char *str);
static gboolean gconf_parse_bool(const char *str);
static GConfValueType gconf_parse_type(int chr);
static const char *gconf_type_repr(GConfValueType type);
static gboolean gconf_require_type(const char *key, const GConfValue *value, GConfValueType type, GError **err);
static gboolean gconf_require_list_type(const char *key, const GConfValue *value, GConfValueType type, GError **err);
gchar *gconf_concat_dir_and_key(const gchar *dir, const gchar *key);
#if GCONF_ENABLE_DEBUG_LOGGING
static char *gconf_value_repr(const char *key, GConfValue *self);
#endif
static char *gconf_value_str(GConfValue *self);
static void gconf_value_unset(GConfValue *self);
static gboolean gconf_value_list_validata(GSList *src, GConfValueType type);
static GSList *gconf_value_list_copy(GSList *src);
static GSList *gconf_value_list_free(GSList *list);
static void gconf_value_set_from_string(GConfValue *self, const char *data);
static GConfValue *gconf_value_init(GConfValueType type, GConfValueType list_type, const char *data);
GConfValue *gconf_value_copy(const GConfValue *src);
GConfValue *gconf_value_new(GConfValueType type);
void gconf_value_free(GConfValue *self);
gboolean gconf_value_get_bool(const GConfValue *self);
void gconf_value_set_bool(GConfValue *self, gboolean val);
int gconf_value_get_int(const GConfValue *self);
void gconf_value_set_int(GConfValue *self, gint val);
double gconf_value_get_float(const GConfValue *self);
void gconf_value_set_float(GConfValue *self, double val);
const char *gconf_value_get_string(const GConfValue *self);
void gconf_value_set_string(GConfValue *self, const char *val);
GConfValueType gconf_value_get_list_type(const GConfValue *self);
void gconf_value_set_list_type(GConfValue *self, GConfValueType list_type);
GSList *gconf_value_get_list(const GConfValue *self);
void gconf_value_set_list(GConfValue *self, GSList *list);
static GConfEntry *gconf_entry_init(const char *key, const char *type, const char *data);
const char *gconf_entry_get_key(const GConfEntry *entry);
GConfValue *gconf_entry_get_value(const GConfEntry *entry);
#if GCONF_ENABLE_DEBUG_LOGGING
static void gconf_client_debug(GConfClient *self);
#endif
GConfClient *gconf_client_get_default(void);
void gconf_client_add_dir(GConfClient *client, const gchar *dir, GConfClientPreloadType preload, GError **err);
static GConfEntry *gconf_client_find_entry(GConfClient *self, const gchar *key, GError **err);
static GConfValue *gconf_client_find_value(GConfClient *self, const gchar *key, GError **err);
GConfValue *gconf_client_get(GConfClient *self, const gchar *key, GError **err);
static void gconf_client_notify_free(GConfClientNotify *self);
static GConfClientNotify *gconf_client_notify_new(const gchar *namespace_section, GConfClientNotifyFunc func, gpointer user_data, GFreeFunc destroy_notify);
static void gconf_client_notify_change(GConfClient *client, const gchar *namespace_section);
guint gconf_client_notify_add(GConfClient *client, const gchar *namespace_section, GConfClientNotifyFunc func, gpointer user_data, GFreeFunc destroy_notify, GError **err);
void gconf_client_notify_remove(GConfClient *client, guint cnxn);
gboolean gconf_client_set_bool(GConfClient *client, const gchar *key, gboolean val, GError **err);
gboolean gconf_client_set_int(GConfClient *client, const gchar *key, gint val, GError **err);
gboolean gconf_client_set_float(GConfClient *client, const gchar *key, double val, GError **err);
gboolean gconf_client_set_string(GConfClient *client, const gchar *key, const gchar *val, GError **err);
gboolean gconf_client_set_list(GConfClient *client, const gchar *key, GConfValueType list_type, GSList *list, GError **err);
void gconf_client_suggest_sync(GConfClient *client, GError **err);

/* ========================================================================= *
 *
 * ERRORS & LOGGING
 *
 * ========================================================================= */

/** Our custom error quark */
static
GQuark
gconf_error_quark (void)
{
  return g_quark_from_static_string ("gconf-setting-error-quark");
}

#define GCONF_ERROR gconf_error_quark()

/** Error logging function */
#if GCONF_ENABLE_ERROR_LOGGING
# define gconf_log_error(FMT, ARG...) mce_log_file(LL_WARN, __FILE__, __FUNCTION__, FMT , ##ARG)
#else
# define gconf_log_error(FMT, ARG...) do{}while(0)
#endif

/** Debug logging function */
#if GCONF_ENABLE_DEBUG_LOGGING
# define gconf_log_debug_p()          mce_log_p(LL_DEBUG)
# define gconf_log_debug(FMT, ARG...) mce_log_file(LL_DEBUG, __FILE__, __FUNCTION__, FMT , ##ARG)
#else
# define gconf_log_debug_p()          0
# define gconf_log_debug(FMT, ARG...) do{}while(0)
#endif

/** Set GError helper */
static
void
gconf_set_error(GError **err, gint code, const gchar *fmt, ...)
__attribute__((format(printf, 3, 4)));

static
void
gconf_set_error(GError **err, gint code, const gchar *fmt, ...)
{
  char *msg = 0;

  va_list va;
  va_start(va, fmt);
  if( vasprintf(&msg, fmt, va) < 0 )
  {
    msg = 0;
  }
  va_end(va);

  /* Assume caller will report error in appropriate context,
   * log from builtin-gconf only in debug verbosity level */
  mce_log(LL_DEBUG, "%s", msg ?: "unknown");

  g_set_error_literal(err, GCONF_ERROR, code, msg ?: "unknown");
  free(msg);
}

/* ========================================================================= *
 *
 * STRING PARSING
 *
 * ========================================================================= */

/** ASCII white character predicate */
static inline gboolean gconf_white_p(int c)
{
  return (c > 0) && (c <= 32);
}

/** ASCII non-white character predicate */
static inline gboolean gconf_black_p(int c)
{
  return (c < 0) || (c > 32);
}

/** Strip excess whitespace from string */
static char *gconf_strip_string(char *str)
{
  if( str )
  {
    char *s = str;
    char *d = str;

    while( gconf_white_p(*s) ) ++s;

    for( ;; )
    {
      while( gconf_black_p(*s) ) *d++ = *s++;

      while( gconf_white_p(*s) ) ++s;

      if( !*s ) break;

      *d++ = ' ';
    }
    *d = 0;
  }
  return str;
}

/** Array of strings we accept as: boolean true */
static const char * const gconf_true_lut[] = { "true", "t", "yes", "y", 0 };

/** Array of strings we accept as: boolean false */
static const char * const gconf_false_lut[] = { "false", "f", "no", "n", 0 };

/** String to int helper */
static int gconf_parse_int(const char *str)
{
  char *end = 0;
  int   res = strtol(str, &end, 0);

  if( (end <= str) || *end )
  {
    gconf_log_error("'%s': is not fully qualified integer", str);
  }
  return res;
}

/** String to double helper */
static double gconf_parse_float(const char *str)
{
  char  *end = 0;
  double res = strtod(str, &end);

  if( (end <= str) || *end )
  {
    gconf_log_error("'%s': is not fully qualified float", str);
  }
  return res;
}

/** String to bool helper */
static gboolean gconf_parse_bool(const char *str)
{
  for( size_t i = 0; gconf_true_lut[i]; ++i )
  {
    if( !strcmp(gconf_true_lut[i], str) ) return TRUE;
  }

  for( size_t i = 0; gconf_false_lut[i]; ++i )
  {
    if( !strcmp(gconf_false_lut[i], str) ) return FALSE;
  }

  int res = gconf_parse_int(str);
  if( res < 0 || res > 1 )
  {
    gconf_log_error("'%s': is not fully qualified bool", str);
  }
  return res;
}

/** Char to GConfValueType helper */
static GConfValueType gconf_parse_type(int chr)
{
  GConfValueType res = GCONF_VALUE_INVALID;

  switch( chr )
  {
  case 'b': res = GCONF_VALUE_BOOL;   break;
  case 'i': res = GCONF_VALUE_INT;    break;
  case 'f': res = GCONF_VALUE_FLOAT;  break;
  case 's': res = GCONF_VALUE_STRING; break;
  case 'a': res = GCONF_VALUE_LIST;   break;

  default:
    gconf_log_error("unknown type '%c'", chr);
    break;
  }
  return res;
}

/* ========================================================================= *
 *
 * MISCELLANEOUS
 *
 * ========================================================================= */

#if GCONF_ENABLE_DEBUG_LOGGING
/** Boolean to text helper */
static
const char *
gconf_bool_repr(gboolean value)
{
  return value ? *gconf_true_lut : *gconf_false_lut;
}
#endif

/** GConfValueType to text helper */
static
const char *
gconf_type_repr(GConfValueType type)
{
  const char *res = "unknown";

  switch( type )
  {
  case GCONF_VALUE_INVALID: res = "invalid"; break;
  case GCONF_VALUE_STRING:  res = "string";  break;
  case GCONF_VALUE_INT:     res = "int";     break;
  case GCONF_VALUE_FLOAT:   res = "float";   break;
  case GCONF_VALUE_BOOL:    res = "bool";    break;
  case GCONF_VALUE_SCHEMA:  res = "schema";  break;
  case GCONF_VALUE_LIST:    res = "list";    break;
  case GCONF_VALUE_PAIR:    res = "pair";    break;
  default: break;
  }

  return res;
}

/** Type checking helper */
static
gboolean
gconf_require_type(const char *key, const GConfValue *value,
                   GConfValueType type, GError **err)
{
  if( value->type == type )
  {
    return TRUE;
  }

  gconf_set_error(err, GCONF_ERROR_TYPE_MISMATCH, "%s: is %s, not %s",
                  key, gconf_type_repr(value->type), gconf_type_repr(type));
  return FALSE;
}

/** List type checking helper */
static
gboolean
gconf_require_list_type(const char *key, const GConfValue *value,
                        GConfValueType type, GError **err)
{
  if( !gconf_require_type(key, value, GCONF_VALUE_LIST, err) )
  {
    return FALSE;
  }
  if( value->list_type == type )
  {
    return TRUE;
  }

  gconf_set_error(err, GCONF_ERROR_TYPE_MISMATCH,
                  "%s: is %s list, not %s list", key,
                  gconf_type_repr(value->list_type), gconf_type_repr(type));
  return FALSE;
}

/** See GConf API documentation */
gchar *
gconf_concat_dir_and_key(const gchar *dir,
                         const gchar *key)
{
  gchar *res = g_strconcat(dir ?: "", "/", key ?: "", NULL);

  if( res != 0 )
  {
    gchar *s = res;
    gchar *d = res;

    while( *s )
    {
      /* copy other than '/' verbatim */
      if( *s != '/' )
      {
        *d++ = *s++;
        continue;
      }

      /* compress '///' into '/' */
      while( *++s == '/' ) {}
      *d++ = '/';
    }

    /* terminate */
    *d = 0;
  }

  return res;
}

/* ========================================================================= *
 *
 * GConfValue
 *
 * ========================================================================= */

#if GCONF_ENABLE_DEBUG_LOGGING
/** GConfValue to text helper
 *
 * The result is for human readable for debugging purposes
 */
static
char *
gconf_value_repr(const char *key, GConfValue *self)
{
  char   *data = 0;
  size_t  size = 0;
  FILE   *file = 0;

  if( !(file = open_memstream(&data, &size)) )
  {
    goto cleanup;
  }

  fprintf(file, "'%s' %s", key, gconf_type_repr(self->type));

  switch( self->type )
  {
  case GCONF_VALUE_STRING:
    fprintf(file, " '%s'", self->data.s);
    break;

  case GCONF_VALUE_INT:
    fprintf(file, " %d", self->data.i);
    break;

  case GCONF_VALUE_FLOAT:
    fprintf(file, " %g", self->data.f);
    break;

  case GCONF_VALUE_BOOL:
    fprintf(file, " %s", gconf_bool_repr(self->data.b));
    break;

  case GCONF_VALUE_SCHEMA:
    fprintf(file, " %g", self->data.f);
    break;

  case GCONF_VALUE_LIST:
    fprintf(file, " of %s [", gconf_type_repr(self->list_type));

    for( GSList *v_iter = self->list_head; v_iter; v_iter = v_iter->next )
    {
      switch( (self = v_iter->data)->type )
      {
      case GCONF_VALUE_STRING:
        fprintf(file, " '%s'", self->data.s);
        break;

      case GCONF_VALUE_INT:
        fprintf(file, " %d", self->data.i);
        break;

      case GCONF_VALUE_FLOAT:
        fprintf(file, " %g", self->data.f);
        break;

      case GCONF_VALUE_BOOL:
        fprintf(file, " %s", gconf_bool_repr(self->data.b));
        break;

      default:
        fprintf(file, " ???");
        break;
      }
    }
    fprintf(file, " ]");
    break;

  case GCONF_VALUE_PAIR:
    fprintf(file, "pair (");
    fprintf(file, " )");
    break;

  case GCONF_VALUE_INVALID:
  default:
    break;
  }

cleanup:
  if( file ) fclose(file);

  return data;
}
#endif

/** GConfValue to text helper
 *
 * The result is compatible with gconf_value_set_from_string()
 */
static
char *
gconf_value_str(GConfValue *self)
{
  char   *data = 0;
  size_t  size = 0;
  FILE   *file = 0;

  if( !(file = open_memstream(&data, &size)) )
  {
    goto cleanup;
  }

  switch( self->type )
  {
  case GCONF_VALUE_STRING:
    fprintf(file, "%s", self->data.s);
    break;

  case GCONF_VALUE_INT:
    fprintf(file, "%d", self->data.i);
    break;

  case GCONF_VALUE_FLOAT:
    fprintf(file, "%g", self->data.f);
    break;

  case GCONF_VALUE_BOOL:
    fprintf(file, "%s", gconf_bool_repr(self->data.b));
    break;

  case GCONF_VALUE_SCHEMA:
    fprintf(file, "%g", self->data.f);
    break;

  case GCONF_VALUE_LIST:
    for( GSList *v_iter = self->list_head; v_iter; v_iter = v_iter->next )
    {
      switch( (self = v_iter->data)->type )
      {
      case GCONF_VALUE_STRING:
        fprintf(file, "%s", self->data.s);
        break;

      case GCONF_VALUE_INT:
        fprintf(file, "%d", self->data.i);
        break;

      case GCONF_VALUE_FLOAT:
        fprintf(file, "%g", self->data.f);
        break;

      case GCONF_VALUE_BOOL:
        fprintf(file, "%s", gconf_bool_repr(self->data.b));
        break;

      default:
        fprintf(file, "???");
        break;
      }
      if( v_iter->next ) {
	fprintf(file, ",");
      }
    }
    break;

  case GCONF_VALUE_PAIR:
    fprintf(file, "???");
    break;
  case GCONF_VALUE_INVALID:
  default:
    break;
  }

cleanup:

  if( file ) fclose(file);

  return data;
}

/** Release dynamic resources of value, but not the value itself */
static
void
gconf_value_unset(GConfValue *self)
{
  // clear the list of values
  for( GSList *elem = self->list_head; elem; elem = elem->next )
  {
    gconf_value_free(elem->data);
  }
  g_slist_free(self->list_head), self->list_head = 0;

  // free any dynamically allocated data
  switch( self->type )
  {
  case GCONF_VALUE_STRING:
    free(self->data.s), self->data.s = 0;
    break;

  default:
    break;
  }
}

/** Helper for type checking a list of values */
static
gboolean
gconf_value_list_validata(GSList *src, GConfValueType type)
{
  for( ; src; src = src->next )
  {
    GConfValue *val = src->data;

    if( !val )
    {
      gconf_log_error("list has NULL value");
      return FALSE;
    }

    if( val->type != type )
    {
      gconf_log_error("list has %s value, expected %s\n",
		      gconf_type_repr(val->type), gconf_type_repr(type));
      return FALSE;
    }
  }
  return TRUE;
}

/** Helper for deep copying list of values */
static
GSList *
gconf_value_list_copy(GSList *src)
{
  GSList *res = 0;

  for( ; src; src = src->next )
  {
    res = g_slist_prepend(res, gconf_value_copy(src->data));
  }
  res = g_slist_reverse(res);

  return res;
}

/** Helper for recursively releasing a list of values */
static
GSList *
gconf_value_list_free(GSList *list)
{
  g_slist_free_full(list, (GDestroyNotify)gconf_value_free);
  return 0;
}

/** Parse value content from text */
static void gconf_value_set_from_string(GConfValue *self, const char *data)
{
  char *now, *zen, *tmp = 0;

  switch( self->type )
  {
  case GCONF_VALUE_BOOL:
    self->data.b = gconf_parse_bool(data);
    break;

  case GCONF_VALUE_INT:
    self->data.i = gconf_parse_int(data);
    break;

  case GCONF_VALUE_FLOAT:
    self->data.f = gconf_parse_float(data);
    break;

  case GCONF_VALUE_STRING:
    self->data.s = strdup(data);
    break;

  case GCONF_VALUE_LIST:
    switch( self->list_type )
    {
    case GCONF_VALUE_BOOL:
    case GCONF_VALUE_INT:
    case GCONF_VALUE_FLOAT:
    case GCONF_VALUE_STRING:
      break;

    default:
      self->list_type = GCONF_VALUE_INVALID;
      self->type      = GCONF_VALUE_INVALID;
      goto cleanup;
    }

    self->list_head = gconf_value_list_free(self->list_head);

    for( now = tmp = strdup(data); now; now = zen )
    {
      if( (zen = strchr(now, ',')) )
      {
        *zen++ = 0;
      }

      GConfValue *elem = gconf_value_init(self->list_type,
                                          GCONF_VALUE_INVALID,
                                          gconf_strip_string(now));
      self->list_head = g_slist_prepend(self->list_head, elem);
    }
    self->list_head = g_slist_reverse(self->list_head);
    break;

  default:
    self->list_type = GCONF_VALUE_INVALID;
    self->type      = GCONF_VALUE_INVALID;
    break;
  }

cleanup:

  free(tmp);
}

/** Create and initialize a GConfValue */
static
GConfValue *
gconf_value_init(GConfValueType type, GConfValueType list_type,
                 const char *data)
{
  GConfValue *self = calloc(1, sizeof *self);

  self->refcount  = 1;
  self->list_type = GCONF_VALUE_INVALID;
  self->type      = type;

  if( self->type == GCONF_VALUE_LIST )
  {
    self->list_type = list_type;
  }

  if( data != 0 )
  {
    gconf_value_set_from_string(self, data);
  }

  return self;
}

/** See GConf API documentation */
GConfValue *
gconf_value_copy(const GConfValue *src)
{
  GConfValue *self = gconf_value_init(src->type, src->list_type, 0);

  if( self->type != src->type || self->list_type != src->list_type )
  {
    goto cleanup;
  }

  switch( self->type )
  {
  case GCONF_VALUE_BOOL:
    self->data.b = src->data.b;
    break;

  case GCONF_VALUE_INT:
    self->data.i = src->data.i;
    break;

  case GCONF_VALUE_FLOAT:
    self->data.f = src->data.f;
    break;

  case GCONF_VALUE_STRING:
    self->data.s = src->data.s ? strdup(src->data.s) : 0;
    break;

  case GCONF_VALUE_LIST:
    self->list_head = gconf_value_list_copy(src->list_head);
    break;

  default:
    self->list_type = GCONF_VALUE_INVALID;
    self->type      = GCONF_VALUE_INVALID;
    break;
  }

cleanup:
  return self;
}

/** See GConf API documentation */
GConfValue *
gconf_value_new(GConfValueType type)
{
  return gconf_value_init(type, GCONF_VALUE_INVALID, 0);
}

/** See GConf API documentation */
void
gconf_value_free(GConfValue *self)
{
  if( self != 0 && --self->refcount == 0 )
  {
    /* get rid of dynamically allocated resources */
    gconf_value_unset(self);

    /* mark the value as invalid to ease stale pointer debugging */
    self->type      = GCONF_VALUE_INVALID;
    self->list_type = GCONF_VALUE_INVALID;

    /* free the value itself too */
    free(self);
  }
}

/** See GConf API documentation */
gboolean
gconf_value_get_bool(const GConfValue *self)
{
  return (self->type == GCONF_VALUE_BOOL) ? self->data.b : 0;
}

/** See GConf API documentation */
void
gconf_value_set_bool(GConfValue *self, gboolean val)
{
  if( self->type == GCONF_VALUE_BOOL )
  {
    self->data.b = val;
  }
  else
  {
    gconf_log_error("not a bool value");
  }
}

/** See GConf API documentation */
int
gconf_value_get_int(const GConfValue *self)
{
  return (self->type == GCONF_VALUE_INT) ? self->data.i : 0;
}

/** See GConf API documentation */
void
gconf_value_set_int(GConfValue *self, gint val)
{
  if( self->type == GCONF_VALUE_INT )
  {
    self->data.i = val;
  }
  else
  {
    gconf_log_error("not an int value");
  }
}

/** See GConf API documentation */
double
gconf_value_get_float(const GConfValue *self)
{
  return (self->type == GCONF_VALUE_FLOAT) ? self->data.f : 0;
}

/** See GConf API documentation */
void
gconf_value_set_float(GConfValue *self, double val)
{
  if( self->type == GCONF_VALUE_FLOAT )
  {
    self->data.f = val;
  }
  else
  {
    gconf_log_error("not a float value");
  }
}

/** See GConf API documentation */
const char *
gconf_value_get_string(const GConfValue *self)
{
  return (self->type == GCONF_VALUE_STRING) ? self->data.s : 0;
}

/** See GConf API documentation */
void
gconf_value_set_string(GConfValue *self, const char *val)
{
  if( self->type == GCONF_VALUE_STRING )
  {
    free(self->data.s), self->data.s = strdup(val);
  }
  else
  {
    gconf_log_error("not a string value");
  }
}

/** See GConf API documentation */
GConfValueType
gconf_value_get_list_type(const GConfValue *self)
{
  return self->list_type;
}

/** See GConf API documentation */
void
gconf_value_set_list_type(GConfValue *self, GConfValueType list_type)
{
  switch( list_type )
  {
  case GCONF_VALUE_STRING:
  case GCONF_VALUE_INT:
  case GCONF_VALUE_FLOAT:
  case GCONF_VALUE_BOOL:
    break;

  default:
    gconf_log_error("list type can't be %s", gconf_type_repr(list_type));
    goto cleanup;
  }

  if( self->type != GCONF_VALUE_LIST )
  {
    gconf_log_error("not a list value");
    goto cleanup;
  }

  if( self->list_type == GCONF_VALUE_INVALID )
  {
    self->list_type = list_type;
  }

  if( self->list_type != list_type )
  {
    gconf_log_error("list type already set");
    goto cleanup;
  }

cleanup:
  return;
}

/** See GConf API documentation */
GSList *
gconf_value_get_list(const GConfValue *self)
{
  return self->list_head;
}

/** See GConf API documentation */
void
gconf_value_set_list(GConfValue *self, GSList *list)
{
  self->list_head = gconf_value_list_free(self->list_head);

  if( gconf_value_list_validata(list, self->list_type) )
  {
    self->list_head = gconf_value_list_copy(list);
  }
}

/* ========================================================================= *
 *
 * GConfEntry
 *
 * ========================================================================= */

/** Create a GConfEntry object */
static
GConfEntry *
gconf_entry_init(const char *key, const char *type, const char *data)
{
  GConfEntry *self = calloc(1, sizeof *self);
  self->key = strdup(key);
  self->def = data ? strdup(data) : 0;

  GConfValueType ltype = GCONF_VALUE_INVALID;
  GConfValueType vtype = gconf_parse_type(type[0]);

  if( vtype == GCONF_VALUE_LIST )
  {
    ltype = gconf_parse_type(type[1]);
  }

  self->value = gconf_value_init(vtype, ltype, data);
  return self;
}

/** See GConf API documentation */
const char *
gconf_entry_get_key(const GConfEntry *entry)
{
  return entry ? entry->key : 0;
}

/** See GConf API documentation */
GConfValue *
gconf_entry_get_value(const GConfEntry *entry)
{
  return entry ? entry->value : 0;
}

/* ========================================================================= *
 *
 * DATABASE
 *
 * ========================================================================= */

/** Built-in set of supported keys and default values */
typedef struct
{
  const char *key;
  const char *type;
  const char *def;
} setting_t;

static const setting_t gconf_defaults[] =
{
  {
    // MCE_GCONF_PSM_PATH @ modules/powersavemode.h
    .key  = "/system/osso/dsm/energymanagement/enable_power_saving",
    .type = "b",
    .def  = "false",
  },
  {
    // MCE_GCONF_FORCED_PSM_PATH @ modules/powersavemode.h
    .key  = "/system/osso/dsm/energymanagement/force_power_saving",
    .type = "b",
    .def  = "false",
  },
  {
    // MCE_GCONF_PSM_THRESHOLD_PATH @ modules/powersavemode.h
    .key  = "/system/osso/dsm/energymanagement/psm_threshold",
    .type = "i",
    .def  = "20",
  },
  {
    // Hint for settings UI. Not used by MCE itself.
    .key  = "/system/osso/dsm/energymanagement/possible_psm_thresholds",
    .type = "ai",
    .def  = "10,20,30,40,50",
  },
  {
    // MCE_GCONF_DISPLAY_ALS_ENABLED_PATH @ modules/filter-brightness-als.h
    .key  = "/system/osso/dsm/display/als_enabled",
    .type = "b",
    .def  = "true",
  },
  {
    // MCE_GCONF_DISPLAY_COLOR_PROFILE_PATH @ modules/filter-brightness-als.h
    .key  = "/system/osso/dsm/display/color_profile",
    .type = "s",
    .def  = "",
  },
  {
    // MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/display_dim_timeout",
    .type = "i",
    .def  = "30",
  },
  {
    // MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/display_blank_timeout",
    .type = "i",
    .def  = "3",
  },
  {
    // MCE_GCONF_DISPLAY_BRIGHTNESS_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/display_brightness",
    .type = "i",
    .def  = "3",
  },
  {
    // Hint for settings UI. Not used by MCE itself.
    .key  = "/system/osso/dsm/display/display_brightness_level_step",
    .type = "i",
    .def  = "1",
  },
  {
    // Hint for settings UI. Not used by MCE itself.
    .key  = "/system/osso/dsm/display/max_display_brightness_levels",
    .type = "i",
    .def  = "5",
  },
  {
    // MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/possible_display_dim_timeouts",
    .type = "ai",
    .def  = "15,30,60,120,180",
  },
  {
    // Hint for settings UI. Not used by MCE itself.
    .key  = "/system/osso/dsm/display/possible_display_blank_timeouts",
    .type = "ai",
    .def  = "3,10,15",
  },
  {
    // MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/use_adaptive_display_dimming",
    .type = "b",
    .def  = "true",
  },
  {
    // MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/adaptive_display_dim_threshold",
    .type = "i",
    .def  = "3000",
  },
  {
    // MCE_GCONF_USE_LOW_POWER_MODE_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/use_low_power_mode",
    .type = "b",
    .def  = "false",
  },
  {
    // MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH @ tklock.h
    .key  = "/system/osso/dsm/locks/touchscreen_keypad_autolock_enabled",
    .type = "b",
    .def  = "true",
  },
  {
    // MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH @ tklock.h
    .key  = "/system/osso/dsm/locks/tklock_double_tap_gesture",
    .type = "i",
    .def  = "1",
  },
  {
    // MCE_GCONF_BLANKING_INHIBIT_MODE_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/inhibit_blank_mode",
    .type = "i",
    .def  = "0",
  },
#if GCONF_ADD_DEBUG_VALUES
  {
    .key  = "/test/string",
    .type = "s",
    .def  = "bar",
  },
  {
    .key  = "/test/string_list",
    .type = "as",
    .def  = "foo,bar,baf",
  },
  {
    .key  = "/test/bool",
    .type = "b",
    .def  = "true",
  },
  {
    .key  = "/test/bool_list",
    .type = "ab",
    .def  = "true,false,1,0,yes,no,t,f,y,n",
  },
  {
    .key  = "/test/int",
    .type = "i",
    .def  = "12345",
  },
  {
    .key  = "/test/int_list",
    .type = "ai",
    .def  = "11,-22,33,-44,55,-66",
  },
  {
    .key  = "/test/float",
    .type = "f",
    .def  = "-123.456",
  },
  {
    .key  = "/test/float_list",
    .type = "af",
    .def  = "0.123,1e2,+42,-42,0.5",
  },
#endif
  {
    // MCE_GCONF_USE_AUTOSUSPEND_PATH @ modules/display.h
    .key  = "/system/osso/dsm/display/autosuspend_policy",
    .type = "i",
    .def  = "1",
  },
  {
    // MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH @ tklock.h
    .key  = "/system/osso/dsm/locks/tklock_blank_disable",
    .type = "i",
    .def  = "0",
  },
#ifdef ENABLE_DOUBLETAP_EMULATION
  {
    // MCE_GCONF_USE_FAKE_DOUBLETAP_PATH @ event-input.h
    .key  = "/system/osso/dsm/event_input/use_fake_double_tap",
    .type = "b",
    .def  = "false",
  },
#endif
  {
    .key = NULL,
  }
};

/* ========================================================================= *
 *
 * GConfClient
 *
 * ========================================================================= */

/** The one and only GConfClient we expect to see */
static GConfClient *default_client = 0;

/** Save values to persistent storage file */
static void gconf_client_save_values(GConfClient *self, const char *path)
{
  char   *data = 0;
  size_t  size = 0;
  FILE   *file = 0;

  mce_log(LL_NOTICE, "updating %s", path);

  if( !(file = open_memstream(&data, &size)) ) {
    goto cleanup;
  }

  for( GSList *e_iter = self->entries; e_iter; e_iter = e_iter->next )
  {
    GConfEntry *entry = e_iter->data;
    char *str = gconf_value_str(entry->value);

    if( !str )
    {
      mce_log(LL_WARN, "failed to serialize value of key %s", entry->key);
      continue;
    }

    /* Omit values that do not differ from defaults */
    if( !entry->def || strcmp(entry->def, str) )
    {
      fprintf(file, "%s=%s\n", entry->key, str);
    }
    free(str);
  }

  // the data pointer gets set at fclose()
  fclose(file), file = 0;

  if( data )
  {
    mce_io_update_file_atomic(path, data, size, 0664, FALSE);
  }

cleanup:

  if( file ) fclose(file);

  free(data);

  return;
}

/** Load values from persistent storage file */
static void gconf_client_load_values(GConfClient *self, const char *path)
{
  FILE  *file = 0;
  char  *buff = 0;
  size_t size = 0;

  mce_log(LL_NOTICE, "loading %s", path);

  if( !(file = fopen(path, "r")) ) {
    if( errno != ENOENT ) {
      mce_log(LL_ERR, "fopen(%s): %m", path);
    }
    goto EXIT;
  }

  while( getline(&buff, &size, file) >= 0 ) {
    char *key = buff;
    char *val = strchr(key, '=');

    if( val ) {
      GError *err = 0;
      GConfEntry *entry;

      *val++ = 0;
      gconf_strip_string(key);
      gconf_strip_string(val);

      if( (entry = gconf_client_find_entry(self, key, &err)) ) {
	gconf_value_set_from_string(entry->value, val);
      }
      g_clear_error(&err);
    }
  }

EXIT:
  free(buff);
  if( file ) fclose(file);
  return;
}

/** Verify that we get only expected GConfClient pointers */
static
gboolean
gconf_client_is_valid(GConfClient *self, GError **err)
{
  if( self == 0 ) {
    gconf_set_error(err, GCONF_ERROR_FAILED, "NULL client passed");
    return FALSE;
  }

  if( self != default_client ) {
    gconf_set_error(err, GCONF_ERROR_FAILED, "Non default client passed");
    return FALSE;
  }

  return TRUE;
}

/** List client values [debugging] */
#if GCONF_ENABLE_DEBUG_LOGGING
static
void
gconf_client_debug(GConfClient *self)
{
  fprintf(stderr, "Values known to builtin-gconf:\n");
  for( GSList *e_iter = self->entries; e_iter; e_iter = e_iter->next )
  {
    GConfEntry *entry = e_iter->data;
    char *repr = gconf_value_repr(entry->key, entry->value);
    fprintf(stderr, "%s\n", repr);
    free(repr);
  }
}
#endif

/** Callback function for logging errors within glob()
 *
 * @param path path to file/dir where error occurred
 * @param err  errno that occurred
 *
 * @return 0 (= do not stop glob)
 */
static int gconf_client_glob_error_cb(const char *path, int err)
{
  mce_log(LL_WARN, "%s: glob: %s", path, g_strerror(err));
  return 0;
}

/** Process config data from /etc/mce/NN.xxx.conf files
 */
static void gconf_client_load_overrides(GConfClient *self)
{
  static const char pattern[] = MCE_CONF_DIR"/[0-9][0-9]*.conf";

  glob_t gb;

  memset(&gb, 0, sizeof gb);

  if( glob(pattern, 0, gconf_client_glob_error_cb, &gb) != 0 )
  {
    mce_log(LL_NOTICE, "no mce config override files found");
    goto cleanup;
  }

  for( size_t i = 0; i < gb.gl_pathc; ++i )
  {
    const char *path = gb.gl_pathv[i];
    gconf_client_load_values(self, path);
  }

cleanup:
  globfree(&gb);
}

/** Set default value state based on the current data
 *
 * The gconf_client_save_values() function will write only
 * keys that have changes since this function was called.
 */
static void gconf_client_mark_defaults(GConfClient *self)
{

  for( GSList *e_iter = self->entries; e_iter; e_iter = e_iter->next )
  {
    GConfEntry *entry = e_iter->data;
    char *str = gconf_value_str(entry->value);

    if( !str )
    {
      mce_log(LL_WARN, "failed to serialize value of key %s", entry->key);
      continue;
    }

    free(entry->def), entry->def = str, str = 0;
  }
}

/** See GConf API documentation */
GConfClient *
gconf_client_get_default(void)
{
  if( default_client == 0 )
  {
    GConfClient *self = calloc(1, sizeof *self);

    // initialize to hard coded defaults
    for( const setting_t *elem = gconf_defaults; elem->key; ++elem )
    {
      GConfEntry *add = gconf_entry_init(elem->key, elem->type, elem->def);
      self->entries = g_slist_prepend(self->entries, add);
    }
    self->entries = g_slist_reverse(self->entries);

    // let gconf_client_is_valid() know about this
    default_client = self;

    // override hard coded defaults via /etc/nn.*.conf
    gconf_client_load_overrides(self);

    // mark down what the state is after hardcoded + overrides
    gconf_client_mark_defaults(self);

    // load custom values
    gconf_client_load_values(self, VALUES_PATH);

    // save back - will be nop unless defaults have changed since last save
    gconf_client_save_values(self, VALUES_PATH);

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      gconf_client_debug(self);
    }
#endif
  }

  return default_client;
}

/** See GConf API documentation */
void
gconf_client_add_dir(GConfClient             *client,
                     const gchar             *dir,
                     GConfClientPreloadType   preload,
                     GError                 **err)
{
  unused(client), unused(err), unused(dir), unused(preload);
  // NOP - not needed to fullfill MCE requirements
  return;
};

/** Locate GConfEntry by key */
static
GConfEntry *
gconf_client_find_entry(GConfClient *self, const gchar *key, GError **err)
{
  GConfEntry *res = 0;

  if( !gconf_client_is_valid(self, err) )
  {
    goto cleanup;
  }

  for( GSList *e_iter = self->entries; e_iter; e_iter = e_iter->next )
  {
    GConfEntry *entry = e_iter->data;
    if( !strcmp(entry->key, key) )
    {
      res = entry;
      break;
    }
  }

  if( !res )
  {
    gconf_set_error(err, GCONF_ERROR_FAILED, "%s: does not exist", key);
  }

cleanup:

  return res;
}

/** Locate GConfValue by key */
static
GConfValue *
gconf_client_find_value(GConfClient *self, const gchar *key, GError **err)
{
  GConfEntry *entry = gconf_client_find_entry(self, key, err);
  return entry ? entry->value : 0;
}

/** See GConf API documentation */
GConfValue *
gconf_client_get(GConfClient *self, const gchar *key, GError **err)
{
  // NOTE: should return a copy of value

  GConfValue *res = gconf_client_find_value(self, key, err);

  if( res )
  {
#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, res);
      gconf_log_debug("GET %s", repr);
      free(repr);
    }
#endif

    /* Since we know that MCE will not modify the GConfValue
     * we return -> skip deep copy and just increase refcount */
    res->refcount += 1;
  }

  return res;
}

/** See GConf API documentation */
gboolean
gconf_client_set_bool(GConfClient *client,
                      const gchar *key,
                      gboolean val,
                      GError **err)
{
  gboolean res = FALSE;
  GConfValue *value = gconf_client_find_value(client, key, err);

  if( value && gconf_require_type(key, value, GCONF_VALUE_BOOL, err) )
  {
    gconf_value_set_bool(value, val);
    res = TRUE;

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, value);
      gconf_log_debug("SET %s", repr);
      free(repr);
    }
#endif

    gconf_client_notify_change(client, key);
  }

  return res;
}

/** See GConf API documentation */
gboolean
gconf_client_set_int(GConfClient *client,
                     const gchar *key,
                     gint val,
                     GError **err)
{
  gboolean res = FALSE;
  GConfValue *value = gconf_client_find_value(client, key, err);

  if( value && gconf_require_type(key, value, GCONF_VALUE_INT, err) )
  {
    gconf_value_set_int(value, val);
    res = TRUE;

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, value);
      gconf_log_debug("SET %s", repr);
      free(repr);
    }
#endif

    gconf_client_notify_change(client, key);
  }
  return res;
}

/** See GConf API documentation */
gboolean
gconf_client_set_float(GConfClient *client,
                       const gchar *key,
                       double val,
                       GError **err)
{
  gboolean res = FALSE;
  GConfValue *value = gconf_client_find_value(client, key, err);

  if( value && gconf_require_type(key, value, GCONF_VALUE_FLOAT, err) )
  {
    gconf_value_set_float(value, val);
    res = TRUE;

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, value);
      gconf_log_debug("SET %s", repr);
      free(repr);
    }
#endif

    gconf_client_notify_change(client, key);
  }
  return res;
}

/** See GConf API documentation */
gboolean
gconf_client_set_string(GConfClient *client,
                        const gchar *key,
                        const gchar *val,
                        GError **err)
{
  gboolean res = FALSE;
  GConfValue *value = gconf_client_find_value(client, key, err);

  if( value && gconf_require_type(key, value, GCONF_VALUE_STRING, err) )
  {
    gconf_value_set_string(value, val);
    res = TRUE;

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, value);
      gconf_log_debug("SET %s", repr);
      free(repr);
    }
#endif

    gconf_client_notify_change(client, key);
  }
  return res;
}

/** See GConf API documentation */
gboolean
gconf_client_set_list(GConfClient *client,
                      const gchar *key,
                      GConfValueType list_type,
                      GSList *list,
                      GError **err)
{
  gboolean res = FALSE;
  GConfValue *value = gconf_client_find_value(client, key, err);

  if( value && gconf_require_list_type(key, value, list_type, err) )
  {
    gconf_value_set_list(value, list);

    res = TRUE;

#if GCONF_ENABLE_DEBUG_LOGGING
    if( gconf_log_debug_p() )
    {
      char *repr = gconf_value_repr(key, value);
      gconf_log_debug("SET %s", repr);
      free(repr);
    }
#endif

    gconf_client_notify_change(client, key);
  }
  return res;
}

/** See GConf API documentation */
void
gconf_client_suggest_sync(GConfClient *client, GError **err)
{
  if( gconf_client_is_valid(client, err) ) {
    // FIXME: do we need delayed save?
    gconf_client_save_values(client, VALUES_PATH);
  }
}

/* ========================================================================= *
 *
 * GConfClientNotify
 *
 * ========================================================================= */

/** Destroy GConfClientNotify object */
static
void
gconf_client_notify_free(GConfClientNotify *self)
{
  // NOTE: explicitly allow gconf_client_notify_free(NULL)

  if( self )
  {
    gconf_log_debug("id=%u, namespace=%s", self->id, self->namespace_section);

    if( self->destroy_notify )
    {
      self->destroy_notify(self->user_data);
    }
    g_free(self->namespace_section);

    free(self);
  }
}

/** Create GConfClientNotify object */
static
GConfClientNotify *
gconf_client_notify_new(const gchar           *namespace_section,
                        GConfClientNotifyFunc  func,
                        gpointer               user_data,
                        GFreeFunc              destroy_notify)
{
  static guint last_id = 0;

  GConfClientNotify *self = calloc(1, sizeof *self);

  self->id = ++last_id;
  self->namespace_section = g_strdup(namespace_section);
  self->func = func;
  self->user_data = user_data;
  self->destroy_notify = destroy_notify;

  gconf_log_debug("id=%u, namespace=%s", self->id, self->namespace_section);

  return self;
}

/** Dispatch change notifications via installed  callbacks */
static
void
gconf_client_notify_change(GConfClient           *client,
                           const gchar           *namespace_section)
{
  GError *err = 0;
  GConfEntry *entry = gconf_client_find_entry(client, namespace_section, &err);

  if( entry )
  {
    for( GSList *item = client->notify_list; item; item = item->next )
    {
      GConfClientNotify *notify = item->data;

      if( notify->func == 0 )
      {
        continue;
      }

      if( !strcmp(notify->namespace_section, namespace_section) )
      {
	gconf_log_debug("id=%u, namespace=%s", notify->id, notify->namespace_section);
        notify->func(client, notify->id, entry, notify->user_data);
      }
    }
  }

  g_clear_error(&err);
}

/** See GConf API documentation */
guint
gconf_client_notify_add(GConfClient *client,
                        const gchar *namespace_section,
                        GConfClientNotifyFunc func,
                        gpointer user_data,
                        GFreeFunc destroy_notify,
                        GError **err)
{
  GConfClientNotify *notify = 0;

  if( !gconf_client_is_valid(client, err) )
  {
    goto cleanup;
  }

  if( gconf_client_find_value(client, namespace_section, err) )
  {
    notify = gconf_client_notify_new(namespace_section,
                                     func, user_data,
                                     destroy_notify);

    client->notify_list = g_slist_prepend(client->notify_list, notify);
  }

cleanup:

  return notify ? notify->id : 0;
}

/** See GConf API documentation */
void
gconf_client_notify_remove(GConfClient *client, guint cnxn)
{
  if( !gconf_client_is_valid(client, 0) )
  {
    goto cleanup;
  }

  for( GSList *item = client->notify_list; item; item = item->next )
  {
    GConfClientNotify *notify = item->data;

    if( notify->id == cnxn )
    {
      gconf_client_notify_free(notify);
      client->notify_list = g_slist_delete_link(client->notify_list, item);
      break;
    }
  }

cleanup:

  return;
}
