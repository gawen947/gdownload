/* File: gdownload.c
   Time-stamp: <2010-03-03 01:03:33 gawen>

   Copyright (C) 2010 David Hauweele <david.hauweele@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <curl/curl.h>

#define VERSION "0.1-git"
#define PACKAGE "gdownload"
#ifndef ARCH
#define ARCH   "(unknown)"
#endif /* ARCH */
#ifndef COMMIT
#define COMMIT "(unknown)"
#endif /* COMMIT */

#ifndef SHARE
#define ICON_PATH "gdownload.png"
#else
#define ICON_PATH SHARE"/gdownload.png"
#endif /* SHARE */

enum defopt  { WIDTH_DEF = 0,
               HEIGHT_DEF = 0,
               POSITION_DEF = GTK_WIN_POS_CENTER };
enum max     { STRLEN_MAX = 1024 };
enum delta   { STATUS_DELTA = 100 };

#define PCT_EPS .01

#ifndef timersub
# define timersub(a, b, result) \
do { \
(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
if ((result)->tv_usec < 0) { \
--(result)->tv_sec; \
(result)->tv_usec += 1000000; \
} \
} while (0)
#endif /* timersub */

#define CTX_T(ptr) ((struct ctx *)ptr)

struct unit
{
  const char *repr;
  const char *desc;
  double factor;
};

struct cmd
{
  const char *name;
  void (*action)(const char *, void *);
  void * ptr;
};

struct s_list
{
  char *string;
  struct s_list *next;
};

struct ctx
{
  const char *name;
  const char *url;
  const char *output;
  const char *referer;
  const char *http_crd;
  const char *proxy;
  const char *proxy_crd;
  const char *intf;
  char *user_agent;
  struct s_list *cookies;
  struct s_list *cks_path;
  struct s_list *cmd_args;
  int dns;
  gint width;
  gint height;
  bool verbose;
  bool status;
  bool progress;
  bool interactive;
  bool fixed;
  bool close_on_finish;
  bool binary;
  struct unit unit;

  int timer;
  double dlnow;
  double dltot;
  struct timeval dl_begin;
  gdouble pct;
  int o_desc;
  CURL *curl;
  GThread *curl_thread;
  bool abort_transfer;
  GtkWidget *gui_progress;
  GtkWidget *gui_status;
  char *pct_progress;
  char *speed_status;
  char *dlnow_status;
  char *dltot_status;
  char *title;
  char *txt_status;
};

struct prefix
{
  const char *repr;
  double value;
};

static struct ctx *global_ctx; /* we just use that for signal handler */
/* any other way ? */

/* TODO: use a header */
static void user_agent(struct ctx *ctx);
static void unload(const struct ctx *ctx);

static void *_xmalloc(size_t size, unsigned int line)
{
  register void *mblk = malloc(size);
  if(mblk)
    return mblk;
  fprintf(stderr,"L%d(%s) Out of memory\n",
          __FILE__,line);
  exit(EXIT_FAILURE);
}
#define xmalloc(size) _xmalloc(size,__LINE__)

static void init_ctx(struct ctx *ctx, const char *name)
{
  memset(ctx,0,sizeof(struct ctx));
  ctx->dns  = CURL_IPRESOLVE_WHATEVER;
  ctx->name = name;
  ctx->width = WIDTH_DEF;
  ctx->height = HEIGHT_DEF;
  ctx->unit.repr = "B";
  ctx->unit.factor = 1.;
  ctx->user_agent   = xmalloc(STRLEN_MAX);
  ctx->txt_status   = xmalloc(STRLEN_MAX);
  ctx->pct_progress = xmalloc(STRLEN_MAX);
  ctx->speed_status = xmalloc(STRLEN_MAX);
  ctx->dlnow_status = xmalloc(STRLEN_MAX);
  ctx->dltot_status = xmalloc(STRLEN_MAX);
  ctx->title        = xmalloc(STRLEN_MAX);
  user_agent(ctx);
}

static struct s_list * add_str(struct s_list *list, const char * str)
{
  register struct s_list *old = list;
  register struct s_list *new = xmalloc(sizeof(struct s_list));
  new->string = xmalloc(strlen(str));
  strcpy(new->string,str);
  new->next = old;
  return new;
}

static void free_s_list(struct s_list *list)
{
  register struct s_list *l;
  for(l = list ; l ; l = l->next) {
    free(l->string);
    free(l);
  }
}

static void free_ctx(struct ctx *ctx)
{
  free_s_list(ctx->cookies);
  free_s_list(ctx->cks_path);
  free_s_list(ctx->cmd_args);
  free(ctx->user_agent);
  free(ctx->pct_progress);
  free(ctx->speed_status);
  free(ctx->dlnow_status);
  free(ctx->dltot_status);
  free(ctx->txt_status);
  free(ctx->title);
}

static void format_nbr(struct ctx *ctx,char *buf, const char *dim, double nbr)
{
  struct prefix *i;
  struct prefix pref[] =
    {
      {"",1.},
      {"K",1E3},
      {"M",1E6},
      {"G",1E9},
      {"T",1E12},
      {"P",1E15},
      {"E",1E18},
      {"Z",1E21},
      {"Y",1E24},
      {NULL,+INFINITY}
    };
  struct prefix binary_pref[] =
    {
      {"i",1.},
      {"Ki",1024},
      {"Mi",1048576},
      {"Gi",1073741824},
      {"Ti",1.09951162778E12},
      {"Pi",1.12589990684E15},
      {"Ei",1.15292150461E18},
      {"Zi",1.18059162072E21},
      {"Yi",1.20892581961E24},
      {NULL,+INFINITY}
    };
  i = ctx->binary ? binary_pref : pref;
  nbr *= ctx->unit.factor;
  for( ; i->repr ; i++) {
    if(nbr < (i+1)->value) {
      snprintf(buf,STRLEN_MAX,"%2.2f %s%s%s",nbr/i->value,
               i->repr,ctx->unit.repr,dim);
      return;
    }
  }
}

static bool is_directory(const char *path)
{
  register DIR *fd;
  fd = opendir(path);
  if(!fd)
    return false;
  closedir(fd);
  return true;
}

static void cmdline(int argc, char *argv[], struct ctx *ctx)
{
  struct option opts[] =
    {
      {"version", no_argument, 0, 'V'},
      {"help", no_argument, 0, 'h'},
      {"verbose", no_argument, 0, 'v'},
      {"status", no_argument, 0, 's'},
      {"progress", no_argument, 0, 'p'},
      {"units", required_argument, 0, 'u'},
      {"binary", no_argument, 0, 'b'},
      {"close", no_argument, 0,'c'},
      {"width", required_argument, 0,'x'},
      {"height", required_argument, 0,'y'},
      {"fixed", no_argument, 0, 'f' },
      {"user-agent", required_argument, 0, 'U'},
      {"referer", required_argument, 0, 'r'},
      {"auth", required_argument, 0, 'a'},
      {"cookies", required_argument, 0, 'C'},
      {"cookies-file", required_argument, 0, 'F'},
      {"proxy", required_argument, 0, 'P'},
      {"proxy-auth", required_argument, 0, 'A'},
      {"ipv4", no_argument, 0, '4'},
      {"ipv6", no_argument, 0, '6'},
      {"intf", required_argument, 0, 'i'},
      {"interactive", no_argument, 0, 'I'},
      {NULL,0,0,0}
    };
  const char *opts_help[] = {
    "Print version information.",
    "Print this message.",
    "Verbose mode.",
    "Show status widget.",
    "Show progress bar widget.",
    "Speed and size unit.",
    "Binary prefix for speed and size.",
    "Close on finish.",
    "Window width.",
    "Window height.",
    "Fixed size.",
    "Set user-agent.",
    "Set referer.",
    "Set HTTP authentification credentials (<user>:<password>).",
    "Set cookies (<name1>:<contents>; <name2>:<contents>; ...).",
    "Set cookies file.",
    "Override proxy settings.",
    "Set proxy authentification credentials (<user>:<password>).",
    "Resolve to IPv4 addresses only.",
    "Resolve to IPv6 only and inhibits IPv4 addresses.",
    "Set outgoing network interface.",
    "Read options from stdin."
  };
  struct unit units[] =
    {
      { "B", "Bytes (8-bits).", 1. },
      { "b", "Bits.", 8. },
      { "dB","Decibels.", 48.1647993062 },
      {NULL, NULL, 0. }
    };
  struct option *opt;
  struct unit *unit;
  const char **hlp;
  int i,c,max,size;
  while(1) {
    c = getopt_long(argc,argv,"Vhvspu:bcx:y:fU:r:a:C:F:P:A:46i:I",opts,NULL);
    if(c == -1)
      break;
    switch(c) {
      case 'V':
        printf(PACKAGE "-" VERSION " (commit:" COMMIT ")\n");
        free_ctx(ctx);
        exit(EXIT_SUCCESS);
      case 'v':
        ctx->verbose = true;
        break;
      case 's':
        ctx->status = true;
        break;
      case 'p':
        ctx->progress = true;
        break;
      case 'u':
        unit = units;
        while(unit->repr && strncmp(optarg,unit->repr,STRLEN_MAX))
          unit++;
        if(!unit->repr) {
          fprintf(stderr,"Units are:\n");
          max = 0;
          for(unit = units ; unit->repr; unit++) {
            size = strlen(unit->repr);
            if(size > max)
              max = size;
          }
          for(unit = units ; unit->repr ; unit++) {
            fprintf(stderr,"  %s",unit->repr);
            size = strlen(unit->repr);
            for( ; size < max ; size++)
              fprintf(stderr," ");
            fprintf(stderr," %s\n",unit->desc);
          }
          free_ctx(ctx);
          exit(EXIT_FAILURE);
        }
        ctx->unit = *unit;
        break;
      case 'b':
        ctx->binary = true;
        break;
      case 'c':
        ctx->close_on_finish = true;
        break;
      case 'x':
        ctx->width = atoi(optarg);
        break;
      case 'y':
        ctx->height = atoi(optarg);
        break;
      case 'f':
        ctx->fixed = true;
        break;
      case 'U':
        strncpy(ctx->user_agent,optarg,STRLEN_MAX);
        break;
      case 'r':
        ctx->referer = optarg;
        break;
      case 'a':
        ctx->http_crd = optarg;
        break;
      case 'C':
        ctx->cookies = add_str(ctx->cookies,optarg);
        break;
      case 'F':
        ctx->cks_path = add_str(ctx->cks_path,optarg);
        break;
      case 'P':
        ctx->proxy = optarg;
        break;
      case 'A':
        ctx->proxy_crd = optarg;
        break;
      case '4':
        ctx->dns = CURL_IPRESOLVE_V4;
        break;
      case '6':
        ctx->dns = CURL_IPRESOLVE_V6;
        break;
      case 'i':
        ctx->intf = optarg;
        break;
      case 'I':
        ctx->interactive = true;
        break;
      case 'h':
      default:
        fprintf(stderr,"Usage: %s [OPTIONS] [URL] [OUTPUT]\n",ctx->name);
        max = 0;
        for(opt = opts ; opt->name; opt++) {
          size = strlen(opt->name);
          if(size > max)
            max = size;
        }
        for(opt = opts, hlp = opts_help ;
            opt->name ;
            opt++,hlp++) {
          fprintf(stderr,"  -%c, --%s",
                  opt->val, opt->name);
          size = strlen(opt->name);
          for( ; size < max ; size++)
            fprintf(stderr," ");
          fprintf(stderr," %s\n",*hlp);
        }
        /* FIXME: print gtk options */
        free_ctx(ctx);
        exit(EXIT_FAILURE);
    }
  }
  if(!(argc-optind) || argc-optind > 2) {
    fprintf(stderr,"Usage: %s [OPTIONS] [URL] [OUTPUT]\n",ctx->name);
    free_ctx(ctx);
    exit(EXIT_FAILURE);
  }
  ctx->url    = argv[optind++];
  ctx->output = (argc - optind) ? argv[optind] : ".";
}

static void user_agent(struct ctx *ctx)
{
  /* FIXME: use strncpy instead and strncat */
  strcpy(ctx->user_agent,"Mozilla/5.0 "
         "(X11 ; U; " ARCH "; commit:" COMMIT "; rv:" VERSION ") (");
  strcat(ctx->user_agent,curl_version());
  strcat(ctx->user_agent,") " PACKAGE "/" VERSION);
}

static const char *extract_path(const char *url)
{
  register const char * n_path;
  n_path = (const char *)strrchr(url,'/');
  n_path = (n_path ? (n_path + 1) : url);
  return n_path;
}

static void load(struct ctx *ctx)
{
  register char * n_path = xmalloc(STRLEN_MAX);
  if(!is_directory(ctx->output))
    strncpy(n_path,ctx->output,STRLEN_MAX);
  else
    snprintf(n_path,STRLEN_MAX,"%s/%s",ctx->output,extract_path(ctx->url));
  snprintf(ctx->title,STRLEN_MAX,"%s - %s",n_path,PACKAGE "-" VERSION);
  ctx->o_desc = creat(n_path,(mode_t)0600);
  free(n_path);
  if(ctx->o_desc != -1)
    return;
  perror("Cannot create output file");
  free_ctx(ctx);
  exit(EXIT_FAILURE);
}

static void *proceed_curl(void *ptr)
{
  int timer = CTX_T(ptr)->timer;
  CURLcode err;
  gettimeofday(&CTX_T(ptr)->dl_begin,NULL); /* FIXME: use a mutex */
  err = curl_easy_perform(CTX_T(ptr)->curl);
  if(timer) {
    gdk_threads_enter();
    g_source_remove(timer);
    gdk_threads_leave();
  }
  if(CTX_T(ptr)->close_on_finish) {
    gdk_threads_enter();
    gtk_main_quit();
    gdk_threads_leave();
  }
  if(err && err != CURLE_ABORTED_BY_CALLBACK)
    fprintf(stderr,"%s\n",curl_easy_strerror(err));
  CTX_T(ptr)->curl_thread = NULL;
  return NULL;
}

static void sigterm(int signal)
{
  register int timer = global_ctx->timer;
  global_ctx->abort_transfer = true;
  if(timer) {
    gdk_threads_enter();
    g_source_remove(timer);
    gdk_threads_leave();
  }
  gdk_threads_enter();
  gtk_main_quit();
  gdk_threads_leave();
}

static gboolean callback_init(gpointer data)
{
  CTX_T(data)->curl_thread = g_thread_create(proceed_curl,
                                             (void *)data,
                                             true,
                                             NULL);
  if (!CTX_T(data)->curl_thread) {
    fprintf(stderr,"Cannot create thread\n");
    gtk_main_quit();
  }
}

static size_t callback_data(void *buffer, size_t size,
                            size_t nmemb, void *userp)
{
  register ssize_t wt = write(CTX_T(userp)->o_desc,buffer,size*nmemb);
  if(wt == -1) {
    perror("Cannot write");
    return size-1;
  }
  return (size_t)wt;
}

static int callback_progress(void *clientp, double dltotal,
                             double dlnow, double ultotal,
                             double ulnow)
{
  /* FIXME: dltotal is quit buggy use wrote byte instead ?*/
  gdouble pct;
  if(dlnow > dltotal)
    pct = 1.;
  else
    pct = dlnow / dltotal;

  if(CTX_T(clientp)->abort_transfer)
    return -1;
  else if(CTX_T(clientp)->progress &&
          fabs(pct - CTX_T(clientp)->pct) > PCT_EPS) {
    CTX_T(clientp)->pct = pct;
    snprintf(CTX_T(clientp)->pct_progress,STRLEN_MAX,"%3.0f%%",100.*pct);
    gdk_threads_enter();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(CTX_T(clientp)->gui_progress),
                                  pct);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(CTX_T(clientp)->gui_progress),
                              CTX_T(clientp)->pct_progress);
    gdk_threads_leave();
  }
  CTX_T(clientp)->dlnow = dlnow;
  CTX_T(clientp)->dltot = dltotal;
  return 0;
}

static gboolean callback_delete(GtkWidget *widget, GdkEvent *event,
                                gpointer data)
{
  register int timer = CTX_T(data)->timer;
  CTX_T(data)->abort_transfer = true;
  if(timer)
    g_source_remove(timer);
  gtk_main_quit();
  return false;
}

static gboolean callback_timer(gpointer data)
{
  double delta;
  struct timeval t_now,t_delta;

  gettimeofday(&t_now,NULL);
  timersub(&t_now,&CTX_T(data)->dl_begin,&t_delta);
  delta = (double)t_delta.tv_sec + (double)t_delta.tv_usec / 1000000;
  format_nbr(CTX_T(data),CTX_T(data)->speed_status, "ps",
             CTX_T(data)->dlnow / delta);
  format_nbr(CTX_T(data),CTX_T(data)->dlnow_status, "",
             CTX_T(data)->dlnow);
  format_nbr(CTX_T(data),CTX_T(data)->dltot_status, "",
             CTX_T(data)->dltot);
  snprintf(CTX_T(data)->txt_status,STRLEN_MAX,"%s (%s/%s)",
           CTX_T(data)->speed_status,
           CTX_T(data)->dlnow_status,
           CTX_T(data)->dltot_status);
  gtk_label_set_text(GTK_LABEL(CTX_T(data)->gui_status),
                     CTX_T(data)->txt_status);
}

static void setup_curl(struct ctx *ctx)
{
  register struct s_list * l;

  curl_global_init(CURL_GLOBAL_ALL);
  ctx->curl = curl_easy_init();

  curl_easy_setopt(ctx->curl,CURLOPT_USERAGENT,ctx->user_agent);
  if(ctx->referer)
    curl_easy_setopt(ctx->curl,CURLOPT_REFERER,ctx->referer);
  if(ctx->http_crd)
    curl_easy_setopt(ctx->curl,CURLOPT_USERPWD,ctx->http_crd);
  for(l = ctx->cookies ; l ; l = l->next)
    curl_easy_setopt(ctx->curl,CURLOPT_COOKIE,l->string);
  for(l = ctx->cks_path ; l ; l = l->next)
    curl_easy_setopt(ctx->curl,CURLOPT_COOKIEFILE,l->string);
  if(ctx->proxy)
    curl_easy_setopt(ctx->curl,CURLOPT_PROXY,ctx->proxy);
  if(ctx->proxy_crd)
    curl_easy_setopt(ctx->curl,CURLOPT_PROXYUSERPWD,ctx->proxy_crd);
  if(ctx->intf)
    curl_easy_setopt(ctx->curl,CURLOPT_INTERFACE,ctx->intf);
  curl_easy_setopt(ctx->curl,CURLOPT_AUTOREFERER,true);
  curl_easy_setopt(ctx->curl,CURLOPT_FOLLOWLOCATION,true);
  curl_easy_setopt(ctx->curl,CURLOPT_FAILONERROR,true);
  curl_easy_setopt(ctx->curl,CURLOPT_IPRESOLVE,ctx->dns);
  curl_easy_setopt(ctx->curl,CURLOPT_VERBOSE,(long)ctx->verbose);
  curl_easy_setopt(ctx->curl,CURLOPT_URL,ctx->url);
  curl_easy_setopt(ctx->curl,CURLOPT_WRITEDATA,ctx);
  curl_easy_setopt(ctx->curl,CURLOPT_WRITEFUNCTION,callback_data);
  if(ctx->progress || ctx->status) {
    curl_easy_setopt(ctx->curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(ctx->curl,CURLOPT_PROGRESSDATA,ctx);
    curl_easy_setopt(ctx->curl,CURLOPT_PROGRESSFUNCTION,callback_progress);
  }
}

static void setup_gui(struct ctx *ctx)
{
  GtkWidget *window,*vbox;

  g_thread_init(NULL);
  gdk_threads_init();

  gtk_init_add(callback_init,ctx);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_signal_connect(G_OBJECT(window),"delete_event",
                   G_CALLBACK(callback_delete),ctx);
  gtk_window_set_role(GTK_WINDOW(window),"gdownload");
  gtk_window_set_title(GTK_WINDOW(window),ctx->title);
  if(ctx->width && ctx->height)
    gtk_window_set_default_size(GTK_WINDOW(window),ctx->width,ctx->height);
  gtk_window_set_position(GTK_WINDOW(window),POSITION_DEF);
  gtk_window_set_resizable(GTK_WINDOW(window),!ctx->fixed);
  gtk_window_set_icon_from_file(GTK_WINDOW(window),ICON_PATH,NULL);
  vbox = gtk_vbox_new(false,0);
  gtk_container_add(GTK_CONTAINER(window),vbox);

  if(ctx->progress) {
    ctx->gui_progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox),ctx->gui_progress,true,true,0);
    gtk_widget_show(ctx->gui_progress);
  }

  if(ctx->status) {
    ctx->timer = g_timeout_add(STATUS_DELTA,callback_timer,ctx);
    ctx->gui_status = gtk_label_new("waiting");
    gtk_label_set_justify(GTK_LABEL(ctx->gui_status),GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox),ctx->gui_status,true,true,0);
    gtk_widget_show(ctx->gui_status);
  }

  gtk_widget_show(vbox);
  gtk_widget_show(window);
}

static void unload(const struct ctx *ctx)
{
  if(close(ctx->o_desc) == -1)
    perror("Cannot close");
}

static void proceed(struct ctx *ctx)
{
  gdk_threads_enter();
  gtk_main();
  gdk_threads_leave();
}

static void handle_signal(struct ctx *ctx)
{
  const int sig[] = { SIGTERM,
                      SIGINT,
                      SIGQUIT,
                      SIGUNUSED };
  const register int * signum;
  global_ctx = ctx;
  for(signum = sig ; *signum != SIGUNUSED ; signum++) {
    if(signal(*signum,sigterm) == SIG_ERR) {
      perror("Cannot handle signal");
      free_ctx(ctx);
      exit(EXIT_FAILURE);
    }
  }
}

static void null_cmd(const char *arg, void *ptr) { return; }
static void true_cmd(const char *arg, void *ptr) { *(bool *)ptr = true; }
static void false_cmd(const char *arg, void *ptr) { *(bool *)ptr = false; }
static void arg_cmd(const char *arg, void *ptr) { *(const char **)ptr = arg; }
static void int_cmd(const char *arg, void *ptr) { *(int *)ptr = atoi(arg); }
static void copy_cmd(const char *arg, void *ptr)
{
  strncpy(*(char **)ptr, arg, STRLEN_MAX);
}
static void append_cmd(const char *arg, void *ptr)
{
*(struct s_list **)ptr = add_str(*(struct s_list **)ptr,arg);
}
static void ipv4_cmd(const char *arg, void *ptr)
{
  *(int *)ptr = CURL_IPRESOLVE_V4;
}
static void ipv6_cmd(const char *arg, void *ptr)
{
  *(int *)ptr = CURL_IPRESOLVE_V6;
}

static void parse_stdin(struct ctx *ctx)
{
  char buf[STRLEN_MAX];
  const char *cmd,*arg;
  struct cmd cmds[] =
    {
      /* url, output (bug with cmdline)*/
      {"verbose", true_cmd, &ctx->verbose},
      {"status", true_cmd, &ctx->status},
      {"progress", true_cmd, &ctx->progress},
      {"binary", true_cmd, &ctx->binary},
      {"close", true_cmd, &ctx->close_on_finish},
      {"width", int_cmd, &ctx->width},
      {"height", int_cmd, &ctx->height},
      {"fixed", true_cmd, &ctx->fixed},
      {"user-agent", copy_cmd, &ctx->user_agent},
      {"referer", arg_cmd, &ctx->referer},
      {"auth", arg_cmd, &ctx->http_crd},
      {"cookie", append_cmd, &ctx->cookies},
      {"cookies-file", append_cmd, &ctx->cks_path},
      {"proxy", arg_cmd, &ctx->proxy},
      {"proxy-auth", arg_cmd, &ctx->proxy_crd},
      {"intf", arg_cmd, &ctx->intf},
      {"ipv4", ipv4_cmd, &ctx->dns},
      {"ipv6", ipv6_cmd, &ctx->dns},
      {"url", arg_cmd, &ctx->url},
      {"output", arg_cmd, &ctx->output},
      {NULL,null_cmd,NULL}
    };
  register struct cmd *c;
  while(fgets(buf,STRLEN_MAX,stdin)) {
    if(buf[0] == '#')
      continue;
    cmd = strtok(buf," \t");
    arg = strtok(NULL,"\n\r\v\f");
    if(strpbrk(cmd,"\n\r\v\f"))
      continue;
    arg = arg ? arg : "";
    ctx->cmd_args = add_str(ctx->cmd_args,arg);
    for(c = cmds ; c->name ; c++) {
      if(!strcmp(cmd,c->name)) {
        c->action(ctx->cmd_args->string,c->ptr);
        break;
      }
    }
    if(!c->name)
      fprintf(stderr,"Syntax error\n");
  }
}

int main(int argc, char *argv[])
{
  struct ctx ctx;
  const char *name;
  name = (const char *)strrchr(argv[0],'/');
  name = name ? (name + 1) : argv[0];
  init_ctx(&ctx,name);
  handle_signal(&ctx);
  gtk_init(&argc,&argv);
  cmdline(argc,argv,&ctx);
  if(ctx.interactive)
    parse_stdin(&ctx);

  load(&ctx);
  setup_gui(&ctx);
  setup_curl(&ctx);

  gdk_threads_enter();
  gtk_main();
  gdk_threads_leave();

  if(ctx.curl_thread)
    g_thread_join(ctx.curl_thread);
  curl_easy_cleanup(ctx.curl);
  curl_global_cleanup();

  unload(&ctx);
  free_ctx(&ctx);
  exit(EXIT_SUCCESS);
}
