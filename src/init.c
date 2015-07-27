#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "common.h"
#include "wsock_trace.h"
#include "bfd_gcc.h"
#include "dump.h"
#include "init.h"

#if !defined(NO_STACK_WALK)
  #include "stkwalk.h"
#endif

struct config_table g_cfg;
int    fatal_error;

CRITICAL_SECTION           crit_sect;
CONSOLE_SCREEN_BUFFER_INFO console_info;

static HANDLE console_hnd = INVALID_HANDLE_VALUE;

static void init_timestamp (void)
{
  LARGE_INTEGER rc;

  QueryPerformanceFrequency (&rc);
  g_cfg.clocks_per_usec = rc.QuadPart / 1000000ULL;
  TRACE (2, "CPU speed: %.3f MHz\n", (double)rc.QuadPart / 1E6);

  QueryPerformanceCounter (&rc);
  g_cfg.start_ticks = rc.QuadPart;
}

static void set_time_format (TS_TYPE *ret, const char *val)
{
  *ret = TS_NONE;

  if (!stricmp(val,"absolute"))
     *ret = TS_ABSOLUTE;
  else if (!stricmp(val,"relative"))
     *ret = TS_RELATIVE;
  else if (!stricmp(val,"delta"))
     *ret = TS_DELTA;
  TRACE (4, "val: %s -> TS_TYPE: %d\n", val, *ret);
}

static const char *get_time_now (void)
{
  static const char *months [12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                   };
  static char time[50];

  GetLocalTime (&g_cfg.start_time);

  snprintf (time, sizeof(time), "%02d %s %d, %02u:%02u:%02u",
            g_cfg.start_time.wDay, months[g_cfg.start_time.wMonth-1], g_cfg.start_time.wYear,
            g_cfg.start_time.wHour, g_cfg.start_time.wMinute, g_cfg.start_time.wSecond);
  return (time);
}

static BOOL image_opt_header_is_msvc (HMODULE mod)
{
  const IMAGE_DOS_HEADER      *dos = (const IMAGE_DOS_HEADER*) mod;
  const IMAGE_NT_HEADERS      *nt  = (const IMAGE_NT_HEADERS*) ((const BYTE*)mod + dos->e_lfanew);
  const IMAGE_OPTIONAL_HEADER *opt = (const IMAGE_OPTIONAL_HEADER*) &nt->OptionalHeader;

  TRACE (2, "opt->MajorLinkerVersion: %u, opt->MinorLinkerVersion: %u\n",
         opt->MajorLinkerVersion, opt->MinorLinkerVersion);
  return (opt->MajorLinkerVersion >= 10 && opt->MinorLinkerVersion == 0);
}

static BOOL image_opt_header_is_mingw (HMODULE mod)
{
  const IMAGE_DOS_HEADER      *dos = (const IMAGE_DOS_HEADER*) mod;
  const IMAGE_NT_HEADERS      *nt  = (const IMAGE_NT_HEADERS*) ((const BYTE*)mod + dos->e_lfanew);
  const IMAGE_OPTIONAL_HEADER *opt = (const IMAGE_OPTIONAL_HEADER*) &nt->OptionalHeader;

  TRACE (2, "opt->MajorLinkerVersion: %u, opt->MinorLinkerVersion: %u\n",
         opt->MajorLinkerVersion, opt->MinorLinkerVersion);
  return (opt->MajorLinkerVersion >= 2 && opt->MajorLinkerVersion < 10);
}

static int config_get_line (FILE *fil, unsigned *line,
                            const char **key_p, const char **val_p)
{
  static char key[256], val[512], val2[512];
  char   *p, *q;
  size_t  len;

  while (1)
  {
    char buf[513];

    if (!fgets(buf,sizeof(buf)-1,fil))   /* EOF */
       return (0);

    for (p = buf; *p && isspace(*p); )
        p++;
    if (*p == '#' || *p == ';' ||
        sscanf(p,"%[^= ] = %[^\r\n]", key, val) != 2)
    {
      (*line)++;
      continue;
    }

    q = strrchr (val, '\"');
    p = strchr (val,';');

    /* Remove trailing comments
     */
    if (p > q)
       *p = '\0';
    p = strchr (val,'#');
    if (p > q)
       *p = '\0';

    /* Remove trailing space.
     */
    for (len = strlen(val)-1; len >= 0 && isspace((int)val[len]); )
        val[len--] = '\0';
    break;
  }

  (*line)++;
  *key_p = key;
  *val_p = getenv_expand (val, val2, sizeof(val2));
  return (1);
}

/*
 * Given a C-format, extract the 1st word from it and check
 * if it should be excluded from tracing. We always assume
 * that 'fmt' starts with a function-name. Using 'strncmp'
 * avoids copying 'fmt' into a local buffer first.
 */
BOOL exclude_list_get (const char *fmt)
{
  size_t i, len;

  /* No tracing of callers should exclude everything.
   */
  if (g_cfg.trace_caller <= 0)
     return (TRUE);

  for (i = 0; i < g_cfg.excl.list_max; i++)
  {
    struct exclude *excl = &g_cfg.excl.func[i];

    len = strlen (excl->name);
    if (!strnicmp(fmt, excl->name, len))
    {
      excl->num_excludes++;
      return (TRUE);
    }
  }
  return (FALSE);
}

BOOL exclude_list_free (void)
{
  size_t i;

  for (i = 0; i < g_cfg.excl.list_max; i++)
  {
    struct exclude *excl = &g_cfg.excl.func[i];
    free (excl->name);
  }
  g_cfg.excl.list_max = 0;
  return (TRUE);
}

/*
 * to-do: Make 'FD_ISSET' an alias for '__WSAFDIsSet'.
 *        pPrint a warning when trying to exclude an unknown Winsock function.
 */
BOOL exclude_list_add (const char *name)
{
  int i = g_cfg.excl.list_max;

  if (!isalpha(*name))
     return (FALSE);

  if (i >= DIM(g_cfg.excl.func)-1)
  {
    TRACE (0, "g_cfg.excl.func[] too small (max. %d elements). val: \"%s\"\n",
              DIM(g_cfg.excl.func), name);
    return (FALSE);
  }
  g_cfg.excl.func[i].name = strdup (name);
  g_cfg.excl.list_max++;
  return (TRUE);
}

/*
 * Open and parse config-file.
 *
 * First try file pointed to by %WSOCK_TRACE,
 * then in current_dir.
 * then in %HOME or %APPDATA.
 */
static void parse_config_file (const char *base_name)
{
  FILE *fil;
  char *home, fname [MAX_PATH];
  char *env = getenv_expand ("WSOCK_TRACE", fname, sizeof(fname));

  TRACE (2, "%%WSOCK_TRACE%%=%s.\n", env);

  if (env == fname)
  {
    if (!FILE_EXISTS(fname))
    {
      WARNING ("%%WSOCK_TRACE=\"%s\" does not exist.\nRunning with default values.\n", env);
      return;
    }
  }
  else
    snprintf (fname, sizeof(fname), "%s\\%s", curr_dir, base_name);

  fil = fopen (fname, "r");
  if (!fil)
  {
    home = getenv ("HOME");
    if (!home)
       home = getenv ("APPDATA");
    if (home)
    {
      snprintf (fname, sizeof(fname), "%s\\%s", home, base_name);
      fil = fopen (fname, "r");
    }
  }

  TRACE (2, "config-file: \"%s\". %sfound.\n", fname, fil ? "" : "not ");

  if (fil)
  {
    const char *key, *val;
    unsigned line = 0;

    str_replace ('\\', '/', fname);

    while (config_get_line(fil,&line,&key,&val))
    {
      TRACE (4, "%s (%u): '%s' = '%s'\n", fname, line, key, val);

      if (!*val)      /* foo = <empty value> */
         continue;

      if (!stricmp(key,"trace_level"))
         g_cfg.trace_level = atoi (val);

      else if (!stricmp(key,"trace_file") && *val)
         g_cfg.trace_file = strdup (val);

      else if (!stricmp(key,"trace_binmode"))
         g_cfg.trace_binmode = atoi (val);

      else if (!stricmp(key,"trace_caller"))
         g_cfg.trace_caller = atoi (val);

      else if (!stricmp(key,"trace_indent"))
      {
        g_cfg.trace_indent = atoi (val);
        g_cfg.trace_indent = max (0, g_cfg.trace_indent);
      }

      else if (!stricmp(key,"trace_report"))
         g_cfg.trace_report = atoi (val);

      else if (!stricmp(key,"trace_max_len"))
         g_cfg.trace_max_len = atoi (val);

      else if (!stricmp(key,"trace_time"))
         set_time_format (&g_cfg.trace_time_format, val);

      else if (!stricmp(key,"pcap_enable"))
         g_cfg.pcap.enable = atoi (val);

      else if (!stricmp(key,"pcap_dump"))
        g_cfg.pcap.dump_fname = strdup (val);

      else if (!stricmp(key,"show_caller"))
         g_cfg.show_caller = atoi (val);

      else if (!stricmp(key,"demangle") || !stricmp(key,"cpp_demangle"))
         g_cfg.cpp_demangle = atoi (val);

      else if (!stricmp(key,"callee_level"))
         g_cfg.callee_level = atoi (val);   /* Control how many stack-frames to show. Not used yet */

      else if (!stricmp(key,"exclude"))
         exclude_list_add (val);

      else if (!stricmp(key,"short_errors"))
         g_cfg.short_errors = atoi (val);

      else if (!stricmp(key,"use_toolhlp32"))
         g_cfg.use_toolhlp32 = atoi (val);

      else if (!stricmp(key,"use_ole32"))
         g_cfg.use_ole32 = atoi (val);

      else if (!stricmp(key,"use_full_path"))
         g_cfg.use_full_path = atoi (val);

      else if (!stricmp(key,"color_file"))
         get_color (val, &g_cfg.color_file);

      else if (!stricmp(key,"color_time"))
         get_color (val, &g_cfg.color_time);

      else if (!stricmp(key,"color_func"))
         get_color (val, &g_cfg.color_func);

      else if (!stricmp(key,"color_trace"))
         get_color (val, &g_cfg.color_trace);

      else if (!stricmp(key,"color_data"))
         get_color (val, &g_cfg.color_data);

      else if (!stricmp(key,"compact"))
         g_cfg.compact = atoi (val);

      else if (!stricmp(key,"dump_select"))
         g_cfg.dump_select = atoi (val);

      else if (!stricmp(key,"dump_nameinfo"))
         g_cfg.dump_nameinfo = atoi (val);

      else if (!stricmp(key,"dump_protoent"))
         g_cfg.dump_protoent = atoi (val);

      else if (!stricmp(key,"dump_hostent"))
         g_cfg.dump_hostent = atoi (val);

      else if (!stricmp(key,"dump_servent"))
         g_cfg.dump_servent = atoi (val);

      else if (!stricmp(key,"dump_data"))
         g_cfg.dump_data = atoi (val);

      else if (!stricmp(key,"dump_wsaprotocol_info"))
         g_cfg.dump_wsaprotocol_info = atoi (val);

      else if (!stricmp(key,"dump_wsanetwork_events"))
         g_cfg.dump_wsanetwork_events = atoi (val);

      else if (!stricmp(key,"max_data"))
         g_cfg.max_data = atoi (val);

      else if (!stricmp(key,"start_new_line"))
         g_cfg.start_new_line = atoi (val);

      else if (!stricmp(key,"test_trace"))
         g_cfg.test_trace = atoi (val);

      else if (!stricmp(key,"msvc_only"))
         g_cfg.msvc_only = atoi (val);

      else if (!stricmp(key,"mingw_only"))
         g_cfg.mingw_only = atoi (val);

      else if (!stricmp(key,"no_buffering"))
         g_cfg.no_buffering = atoi (val);

      else
         TRACE (0, "%s (%u):\nUnknown keyword '%s' = '%s'\n",
                fname, line, key, val);

      /* to-do: handle more 'key' / 'val' here */
    }
    fclose (fil);
  }
}

static void trace_report (void)
{
  const struct exclude *ex;
  const char  *indent;
  unsigned     i;
  size_t       len, max_len = 0;

  g_cfg.trace_report = FALSE;

  trace_puts ("\n  Exclusions:~5");

  for (i = 0; i < g_cfg.excl.list_max; i++)
  {
    ex = &g_cfg.excl.func[i];
    len = strlen (ex->name);
    if (max_len < len)
       max_len = len;
  }
  if (i == 0)
    trace_puts (" None.\n");
  else
  {
    for (i = 0; i < g_cfg.excl.list_max; i++)
    {
      indent = (i == 0) ? " " : "              ";
      ex = &g_cfg.excl.func[i];
      len = strlen (ex->name);
      trace_printf ("%s%s():%*s %3d times.\n",
                    indent, ex->name, max_len-len, "", ex->num_excludes);
    }
  }

  if (g_cfg.reentries > 0)
     trace_printf ("  get_caller() reentered %lu times.\n", g_cfg.reentries);

  if (g_cfg.counts.dll_attach > 0 || g_cfg.counts.dll_detach > 0)
  {
    trace_printf ("  DLL attach %"U64_FMT " times.\n", g_cfg.counts.dll_attach);
    trace_printf ("  DLL detach %"U64_FMT " times.\n", g_cfg.counts.dll_detach);
  }

 /* E.g.:
  *  Statistics:
  *    Recv bytes:   1,000,000,000  Recv errors:           0
  *    Recv bytes:   9,999,999,900 (MSG_PEEK)
  *    Send bytes:      20,000,000  Send errors:      20,000
  */

#if 0  /* test */
  g_cfg.counts.recv_bytes  = 1000000000;
  g_cfg.counts.recv_peeked = 9999999900;
  g_cfg.counts.send_bytes  = 20000000;
  g_cfg.counts.send_errors = 20000;
#endif

  trace_printf ("~0\n"
                "  Statistics:\n"
                "    Recv bytes:  %15s",               qword_str(g_cfg.counts.recv_bytes));
  trace_printf ("  Recv errors: %15s\n",               qword_str(g_cfg.counts.recv_errors));
  trace_printf ("    Recv bytes:  %15s  (MSG_PEEK)\n", qword_str(g_cfg.counts.recv_peeked));
  trace_printf ("    Send bytes:  %15s",               qword_str(g_cfg.counts.send_bytes));
  trace_printf ("  Send errors: %15s\n",               qword_str(g_cfg.counts.send_errors));
}

/*
 * Called from DllMain(): dwReason == DLL_PROCESS_DETACH
 */
void wsock_trace_exit (void)
{
  set_color (NULL);

  if (fatal_error)
     g_cfg.trace_report = FALSE;

#if 0
  if (!cleaned_up || startup_count > 0)
     g_cfg.trace_report = FALSE;
#endif

  if (g_cfg.trace_report)
     trace_report();

  exclude_list_free();

#if !defined(NO_STACK_WALK)
  StackWalkExit();
#endif

  common_exit();

  if (g_cfg.trace_stream)
    fclose (g_cfg.trace_stream);

  g_cfg.trace_file_okay = FALSE;
  g_cfg.trace_stream = NULL;

  if (g_cfg.trace_file)
     free (g_cfg.trace_file);

  if (g_cfg.pcap.dump_fname)
     free (g_cfg.pcap.dump_fname);

  DeleteCriticalSection (&crit_sect);
}

/*
 * Called from DllMain(): dwReason == DLL_PROCESS_ATTACH
 */
void wsock_trace_init (void)
{
  char       *end;
  const char *now;
  BOOL        okay;
  HMODULE     mod;
  BOOL        is_msvc, is_mingw;
//static char buf [256];

  InitializeCriticalSection (&crit_sect);

//QueryPerformanceCounter (&g_cfg.self_ticks);

  /* Set default values.
   */
  memset (&g_cfg, sizeof(g_cfg), 0);
  g_cfg.trace_level   = 1;
  g_cfg.trace_max_len = 9999;      /* Infinite */
  g_cfg.screen_width  = g_cfg.trace_max_len;
  g_cfg.trace_stream  = stdout;
  g_cfg.trace_file_device = TRUE;

  common_init();

  mod = GetModuleHandle (NULL);
  GetCurrentDirectory (sizeof(curr_dir), curr_dir);
  GetModuleFileName (NULL, prog_dir, sizeof(prog_dir));
  end = strrchr (prog_dir, '\0');
  if (!strnicmp(end-4,".exe",4))
  {
    end = strrchr (prog_dir, '\\');
    _strlcpy (curr_prog, end+1, sizeof(curr_prog));
    end[1] = '\0';  /* Ensure 'prog_dir' has a trailing '\\' */
  }
  else
    _strlcpy (curr_prog, "??", sizeof(curr_prog));

  parse_config_file ("wsock_trace");

  is_msvc  = image_opt_header_is_msvc (mod);
  is_mingw = image_opt_header_is_mingw (mod);

  /* Should test on WSOCK_TRACE_DLL name too.
   */
  if ((g_cfg.msvc_only  && !is_msvc) ||
      (g_cfg.mingw_only && !is_mingw) )
  {
 // g_cfg.stealth_mode = 1;
    g_cfg.trace_level = g_cfg.trace_report = 0;
  }

  if (g_cfg.trace_file && !stricmp(g_cfg.trace_file,"stderr"))
  {
    g_cfg.trace_stream      = stderr;
    g_cfg.trace_file_device = TRUE;
  }
  else if (g_cfg.trace_file && !stricmp(g_cfg.trace_file,"$ODS"))
  {
    g_cfg.trace_stream      = NULL;
    g_cfg.trace_file_device = TRUE;
    g_cfg.trace_use_ods     = TRUE;
    trace_binmode = 1;
  }
  else if (g_cfg.trace_file && g_cfg.trace_level > 0)
  {
    g_cfg.trace_stream      = fopen_excl (g_cfg.trace_file, "at+");
    g_cfg.trace_file_okay   = (g_cfg.trace_stream != NULL);
    g_cfg.trace_file_device = FALSE;

    if (!g_cfg.trace_stream || !FILE_EXISTS(g_cfg.trace_file))
    {
      WARNING ("Failed to open or create trace_file '%s': %s\n"
               "Printing to stdout.\n", g_cfg.trace_file, strerror(errno));
      g_cfg.trace_stream = stdout;
      g_cfg.trace_file = NULL;
    }
  }

  if (g_cfg.trace_stream)
  {
    if (g_cfg.no_buffering)
       setvbuf (g_cfg.trace_stream, NULL, _IONBF, 0);

    if (g_cfg.trace_binmode)
    {
      _setmode (_fileno(g_cfg.trace_stream), O_BINARY);
      trace_binmode = 1;
    }
  }

  if (g_cfg.pcap.enable)
  {
    g_cfg.pcap.dump_stream = fopen_excl (g_cfg.pcap.dump_fname, "w+b");
    write_pcap_header();
  }

  now = get_time_now();

  if (g_cfg.trace_level > 0 &&
      (g_cfg.trace_use_ods || (!g_cfg.trace_file_device && g_cfg.trace_file_okay)))
    trace_printf ("\n------- Trace started at %s --------"
                  "--------------------------\n", now);

  memset (&console_info, 0, sizeof(console_info));

  if (g_cfg.trace_stream == stderr)
       console_hnd = GetStdHandle (STD_ERROR_HANDLE);
  else console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);

  okay = (console_hnd != INVALID_HANDLE_VALUE &&
          GetConsoleScreenBufferInfo(console_hnd, &console_info));

  if (!okay || GetFileType(console_hnd) != FILE_TYPE_CHAR)
     g_cfg.stdout_redirected = TRUE;

  if (!g_cfg.stdout_redirected)
  {
    const char *env = getenv ("LINES");

    if (env && atoi(env) > 0)
         g_cfg.screen_heigth = atoi (env);
    else g_cfg.screen_heigth = console_info.srWindow.Bottom - console_info.srWindow.Top + 1;

    env = getenv ("COLUMNS");

    if (env && atoi(env) > 0)
         g_cfg.screen_width = atoi (env);
    else g_cfg.screen_width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
  }
  else
    g_cfg.screen_width = g_cfg.trace_max_len;

  TRACE (2, "g_cfg.screen_width: %d, g_cfg.screen_heigth: %d, g_cfg.stdout_redirected: %d\n",
            g_cfg.screen_width, g_cfg.screen_heigth, g_cfg.stdout_redirected);

  TRACE (2, "g_cfg.trace_file_okay: %d, g_cfg.trace_file_device: %d\n",
            g_cfg.trace_file_okay, g_cfg.trace_file_device);

  if (!g_cfg.stdout_redirected)
  {
    if (!g_cfg.color_file)
       g_cfg.color_file = console_info.wAttributes;

    if (!g_cfg.color_func)
       g_cfg.color_func = console_info.wAttributes;

    if (!g_cfg.color_trace)
       g_cfg.color_trace = console_info.wAttributes;

    if (!g_cfg.color_data)
       g_cfg.color_data = console_info.wAttributes;
  }

  if (g_cfg.trace_level == 0)
     g_cfg.dump_data = g_cfg.dump_select = 0;

  if (g_cfg.trace_time_format != TS_NONE)
     init_timestamp();

  if (g_cfg.trace_level <= 0)
  {
    g_cfg.dump_data     = FALSE;
    g_cfg.dump_hostent  = FALSE;
    g_cfg.dump_servent  = FALSE;
    g_cfg.dump_protoent = FALSE;
    g_cfg.dump_nameinfo = FALSE;
    g_cfg.dump_wsaprotocol_info  = FALSE;
    g_cfg.dump_wsanetwork_events = FALSE;
  }

  TRACE (3, "curr_prog: '%s', curr_dir: '%s'\n"
            "  prog_dir: '%s'\n", curr_prog, curr_dir, prog_dir);

  if (g_cfg.trace_level >= 3)
     check_all_search_lists();

  load_ws2_funcs();

#if defined(USE_BFD)
  BFD_init();
#endif

#if !defined(NO_STACK_WALK)
  StackWalkInit();
#endif
}

/*
 * Used as e.g. 'INIT_PTR (p_WSAStartup)' which expands to
 *   'init_ptr ((const void**)&p_WSAStartup, "p_WSAStartup")'.
 * Hence 'func_name' should be 'ptr_name + 2'.
 */
void init_ptr (const void **ptr, const char *ptr_name)
{
  const char *func_name = ptr_name + 2;

  if (*ptr == NULL)
  {
    const struct LoadTable *f = find_ws2_func_by_name (func_name);

    if (f && f->optional)
    {
      TRACE (1, "Optional function '%s()' not found. Continuing anyway.\n", func_name);
      return;
    }
    FATAL ("Function '%s()' not initialised.\n", func_name);
  }

  if (cleaned_up)
  {
    TRACE (0, "Function '%s()' called after 'WSACleanup()' was done.\n", func_name);
//  exit (-1);
  }
}

static const struct search_list colors[] = {
                              { 0, "black"   },
                              { 1, "blue"    },
                              { 2, "green"   },
                              { 3, "cyan"    },
                              { 4, "red"     },
                              { 5, "magenta" },
                              { 6, "yellow"  },
                              { 7, "white"   }
                            };
/*
 * Parse a color specifier like:
 *   [bright] fg [on bg]
 *
 * Returns foreground in lower 8 bits of '*col'.
 *     and background in upper 8 bits of '*col'.
 * Sets upper 8 bits in '*col' to -1 if "on bg" is missing.
 * I.e. using default background in set_color().
 */
void get_color (const char *val, WORD *col)
{
  BYTE        fg = 0;
  BYTE        bg = (BYTE)-1;
  unsigned    x;
  const char *orig = val;
  char        fg_str [20] = "";
  char        bg_str [20] = "";
  int         num1, num2;

  if (!strnicmp(val,"bright ",7))
  {
    fg |= FOREGROUND_INTENSITY;
    val += 7;
  }

  num1 = sscanf (val, "%20s", fg_str);
  num2 = sscanf (val, "%20[^ ] on %20s", fg_str, bg_str);

  if (num1 != 1 && num2 != 2)
  {
    TRACE (0, "Unknown color '%s'\n", orig);
    return;
  }

  TRACE (4, "num1: %d, num2: %d, fg_str: '%s', bg_str: '%s'\n", num1, num2, fg_str, bg_str);

  x = list_lookup_value (fg_str, colors, DIM(colors));
  if (x == UINT_MAX)
  {
    TRACE (0, "Unknown color '%s'\n", orig);
    return;
  }
  fg |= x;

  if (bg_str[0])
  {
    x = list_lookup_value (bg_str, colors, DIM(colors));
    if (x == UINT_MAX)
    {
      TRACE (0, "Unknown color '%s'\n", orig);
      return;
    }

   /* Since BG with high intensity (BACKGROUND_INTENSITY) isn't supported.
    */
    bg = x;
  }

  *col = fg + (bg << 8);

  if (!g_cfg.trace_use_ods && g_cfg.trace_file_device)
     TRACE (4, "orig '%s' -> fg: %d, bg: %d, *col: %u\n", orig, (int)fg, (int)bg, *col);
}

/*
 * Set console foreground and optionally background color.
 * FG is in the low 4 bits.
 * BG is in the upper 4 bits of the BYTE.
 * If 'col == NULL', set default console colour.
 */
void set_color (const WORD *col)
{
  BYTE   fg, bg;
  WORD   attr;
  static WORD last_attr = (WORD)-1;

  if (!col)
  {
    attr = console_info.wAttributes;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);
  }
  else
  {
    attr = *col;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);

    if (bg == (BYTE)-1)
         attr = console_info.wAttributes & ~7;
    else attr = bg << 4;

    attr |= fg;
  }

  if (!g_cfg.trace_use_ods && g_cfg.trace_file_device)
     TRACE (6, "fg: %d, bg: %d, attr: 0x%04X\n", (int)fg, (int)bg, attr);

  if (attr != last_attr)
     SetConsoleTextAttribute (console_hnd, attr);
  last_attr = attr;
}

int get_column (void)
{
  CONSOLE_SCREEN_BUFFER_INFO ci;

  if (console_hnd == INVALID_HANDLE_VALUE)
     return (-1);

  memset (&ci, 0, sizeof(ci));
  if (!GetConsoleScreenBufferInfo (console_hnd, &ci))
     return (-1);

  return (int) (ci.dwCursorPosition.X);
}


/*
 * Functions for writing dump-file in pcap-format.
 */
#define TCPDUMP_MAGIC       0xA1B2C3D4
#define PCAP_VERSION_MAJOR  2
#define PCAP_VERSION_MINOR  4
#define DLT_NULL            0     /* BSD loopback encapsulation */
#define DLT_RAW             14    /* raw IP */
#define DLT_IPV4            228
#define DLT_IPV6            229
#define PROTO_TCP           6     /* on network order */
#define PROTO_UDP           17    /* on network order */

#if defined(_MSC_VER) || defined(__CYGWIN__)
  #pragma pack(push,1)
#else
  #pragma pack(1)
#endif

struct pcap_file_header {
       DWORD  magic;
       WORD   version_major;
       WORD   version_minor;
       DWORD  thiszone;        /* GMT to local correction */
       DWORD  sigfigs;         /* accuracy of timestamps */
       DWORD  snap_len;        /* max length saved portion of each pkt */
       DWORD  linktype;        /* data link type (DLT_*) */
     };

/*
 * Each packet in the dump file is prepended with this generic header.
 * This gets around the problem of different headers for different
 * packet interfaces.
 */
struct pcap_pkt_header {
       struct timeval ts;      /* time stamp */
       DWORD          caplen;  /* length of portion present */
       DWORD          len;     /* length of this packet (off wire) */
       DWORD          family;  /* protocol family value (for DLT_NULL) */
     };

struct ip_header {
       BYTE    ip_hlen : 4;    /* header length */
       BYTE    ip_ver  : 4;    /* version 4 */
       BYTE    ip_tos;         /* type of service */
       WORD    ip_len;         /* total length */
       WORD    ip_id;          /* identification */
       WORD    ip_off;         /* fragment offset field */
       BYTE    ip_ttl;         /* time to live */
       BYTE    ip_p;           /* protocol */
       WORD    ip_sum;         /* checksum */
       DWORD   ip_src;         /* source address */
       DWORD   ip_dst;         /* dest address */
     };


#if defined(_MSC_VER) || defined(__CYGWIN__)
  #pragma pack(pop)
#else
  #pragma pack()
#endif


static const void *make_ip_hdr (size_t data_len)
{
  static struct ip_header ip;
  static WORD   ip_id = 1;

  memset (&ip, 0, sizeof(ip));
  ip.ip_hlen = sizeof(ip) / 4;
  ip.ip_ver = 4;
  ip.ip_len = swap16 ((WORD)data_len);
  ip.ip_ttl = (BYTE) 256;
  ip.ip_id  = ip_id++;
  ip.ip_p   = PROTO_TCP;
  return (&ip);
}

// to-do

static const void *make_udp_hdr (int data_len)
{
  return (NULL);
}

static const void *make_tcp_hdr (int data_len)
{
  return (NULL);
}

/*
 * Stolen and modified from APR (Apache Portable Runtime):
 * Number of micro-seconds between the beginning of the Windows epoch
 * (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970).
 *
 * This assumes all Win32 compilers have 64-bit support.
 */
#define DELTA_EPOCH_IN_USEC  U64_SUFFIX (11644473600000000)

static __inline uint64 FileTimeToUnixEpoch (const FILETIME *ft)
{
  uint64 res = (uint64) ft->dwHighDateTime << 32;

  res |= ft->dwLowDateTime;
  res /= 10;                   /* from 100 nano-sec periods to usec */
  res -= DELTA_EPOCH_IN_USEC;  /* from Win epoch to Unix epoch */
  return (res);
}

static void _gettimeofday (struct timeval *tv)
{
  FILETIME ft;
  uint64   tim;

  GetSystemTimeAsFileTime (&ft);
  tim = FileTimeToUnixEpoch (&ft);
  tv->tv_sec  = (long) (tim / 1000000L);
  tv->tv_usec = (long) (tim % 1000000L);
}

#if defined(__WATCOMC__)
  #define _timezone (*__get_timezone_ptr())
#endif

size_t write_pcap_header (void)
{
  struct pcap_file_header pf_hdr;
  size_t rc;

  if (!g_cfg.pcap.dump_stream)
     return (-1);

  memset (&pf_hdr, 0, sizeof(pf_hdr));

  pf_hdr.magic         = TCPDUMP_MAGIC;
  pf_hdr.version_major = PCAP_VERSION_MAJOR;
  pf_hdr.version_minor = PCAP_VERSION_MINOR;
  pf_hdr.thiszone      = 60 * _timezone;
  pf_hdr.sigfigs       = 0;
  pf_hdr.snap_len      = 64*1024;
  pf_hdr.linktype      = DLT_RAW;

  rc = fwrite (&pf_hdr, 1, sizeof(pf_hdr), g_cfg.pcap.dump_stream);
  return (rc == 0 ? -1 : rc);
}

size_t write_pcap_packet (const void *pkt, size_t len, BOOL out)
{
  struct pcap_pkt_header pc_hdr;
  size_t rc, pcap_len;

  if (!g_cfg.pcap.dump_stream)
     return (-1);

  pcap_len = len + sizeof(struct ip_header) /* + sizeof(struct tcp_header) */ ;
  _gettimeofday (&pc_hdr.ts);

  pc_hdr.len    = (DWORD) pcap_len + sizeof(pc_hdr.family);
  pc_hdr.caplen = (DWORD) pcap_len;
  pc_hdr.family = swap32 (AF_INET);

  fwrite (&pc_hdr, sizeof(pc_hdr), 1, g_cfg.pcap.dump_stream);
  fwrite (make_ip_hdr(len), sizeof(struct ip_header), 1, g_cfg.pcap.dump_stream);
//fwrite (make_tcp_hdr(len), sizeof(struct tcp_header), 1, g_cfg.pcap.dump_stream);

  rc = fwrite (pkt, 1, len, g_cfg.pcap.dump_stream);
  return (rc == 0 ? -1 : pcap_len);
}

#if defined(_MSC_VER) && defined(_DEBUG)
static _CrtMemState last_state;

void crtdbg_init (void)
{
  int flags = _CRTDBG_LEAK_CHECK_DF |
              _CRTDBG_DELAY_FREE_MEM_DF |
           /* _CRTDBG_CHECK_CRT_DF | */   /* Don't report allocs in CRT */
              _CRTDBG_CHECK_ALWAYS_DF |
              _CRTDBG_ALLOC_MEM_DF;

  _CrtSetReportFile (_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode (_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetDbgFlag (flags | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG));
  _CrtMemCheckpoint (&last_state);
}

void crtdbg_exit (void)
{
  _CrtMemDumpAllObjectsSince (&last_state);
  _CrtMemDumpStatistics (&last_state);
  _CrtCheckMemory();
  _CrtDumpMemoryLeaks();
}

#else
  void crtdbg_init (void) {}
  void crtdbg_exit (void) {}
#endif