/* A Bison parser, made by GNU Bison 3.0.4.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.0.4"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 1

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* Copy the first part of user declarations.  */
#line 1 "src/ostree/parse-datetime.y" /* yacc.c:339  */

/* Parse a string into an internal time stamp.

   Copyright (C) 1999-2000, 2002-2015 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published
   by the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Originally written by Steven M. Bellovin <smb@research.att.com> while
   at the University of North Carolina at Chapel Hill.  Later tweaked by
   a couple of people on Usenet.  Completely overhauled by Rich $alz
   <rsalz@bbn.com> and Jim Berets <jberets@bbn.com> in August, 1990.

   Modified by Paul Eggert <eggert@twinsun.com> in August 1999 to do
   the right thing about local DST.  Also modified by Paul Eggert
   <eggert@cs.ucla.edu> in February 2004 to support
   nanosecond-resolution time stamps, and in October 2004 to support
   TZ strings in dates.  */

/* FIXME: Check for arithmetic overflow in all cases, not just
   some of them.  */


#include "config.h"
#include "parse-datetime.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <glib.h>
#include <stdint.h>

/* There's no need to extend the stack, so there's no need to involve
   alloca.  */
#define YYSTACK_USE_ALLOCA 0

static char *
xmemdup (void const *p, size_t s)
{
  char *result = g_malloc (s);
  memcpy (result, p, s);
  return result;
}

static void
gettime (struct timespec *ts)
 {
#ifdef HAVE_NANOTIME
   nanotime (ts);
#else

# if defined(CLOCK_REALTIME) && defined(HAVE_CLOCK_GETTIME)
   if (clock_gettime (CLOCK_REALTIME, ts) == 0)
     return;
# endif

   {
     struct timeval tv;
     gettimeofday (&tv, NULL);
     ts->tv_sec = tv.tv_sec;
     ts->tv_nsec = tv.tv_usec * 1000;
   }

#endif
 }

/* Tell Bison how much stack space is needed.  20 should be plenty for
   this grammar, which is not right recursive.  Beware setting it too
   high, since that might cause problems on machines whose
   implementations have lame stack-overflow checking.  */
#define YYMAXDEPTH 20
#define YYINITDEPTH YYMAXDEPTH

/* Since the code of parse-datetime.y is not included in the Emacs executable
   itself, there is no need to #define static in this file.  Even if
   the code were included in the Emacs executable, it probably
   wouldn't do any harm to #undef it here; this will only cause
   problems if we try to write to a static variable, which I don't
   think this code needs to do.  */
#ifdef emacs
# undef static
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Bison's skeleton tests _STDLIB_H, while some stdlib.h headers
   use _STDLIB_H_ as witness.  Map the latter to the one bison uses.  */
/* FIXME: this is temporary.  Remove when we have a mechanism to ensure
   that the version we're using is fixed, too.  */
#ifdef _STDLIB_H_
# undef _STDLIB_H
# define _STDLIB_H 1
#endif

/* ISDIGIT differs from isdigit, as follows:
   - Its arg may be any int or unsigned int; it need not be an unsigned char
     or EOF.
   - It's typically faster.
   POSIX says that only '0' through '9' are digits.  Prefer ISDIGIT to
   isdigit unless it's important to use the locale's definition
   of "digit" even when the host does not conform to POSIX.  */
#define ISDIGIT(c) ((unsigned int) (c) - '0' <= 9)

/* Shift A right by B bits portably, by dividing A by 2**B and
   truncating towards minus infinity.  A and B should be free of side
   effects, and B should be in the range 0 <= B <= INT_BITS - 2, where
   INT_BITS is the number of useful bits in an int.  GNU code can
   assume that INT_BITS is at least 32.

   ISO C99 says that A >> B is implementation-defined if A < 0.  Some
   implementations (e.g., UNICOS 9.0 on a Cray Y-MP EL) don't shift
   right in the usual way when A < 0, so SHR falls back on division if
   ordinary A >> B doesn't seem to be the usual signed shift.  */
#define SHR(a, b)       \
  (-1 >> 1 == -1        \
   ? (a) >> (b)         \
   : (a) / (1 << (b)) - ((a) % (1 << (b)) < 0))

#define EPOCH_YEAR 1970
#define TM_YEAR_BASE 1900

#define HOUR(x) ((x) * 60)

/* Convert a possibly-signed character to an unsigned character.  This is
   a bit safer than casting to unsigned char, since it catches some type
   errors that the cast doesn't.  */
static unsigned char to_uchar (char ch) { return ch; }

/* FIXME: It also assumes that signed integer overflow silently wraps around,
   but this is not true any more with recent versions of GCC 4.  */

/* An integer value, and the number of digits in its textual
   representation.  */
typedef struct
{
  bool negative;
  long int value;
  size_t digits;
} textint;

/* An entry in the lexical lookup table.  */
typedef struct
{
  char const *name;
  int type;
  int value;
} table;

/* Meridian: am, pm, or 24-hour style.  */
enum { MERam, MERpm, MER24 };

enum { BILLION = 1000000000, LOG10_BILLION = 9 };

/* Relative times.  */
typedef struct
{
  /* Relative year, month, day, hour, minutes, seconds, and nanoseconds.  */
  long int year;
  long int month;
  long int day;
  long int hour;
  long int minutes;
  intmax_t seconds;
  int ns;
} relative_time;

#define RELATIVE_TIME_0 ((relative_time) { 0, 0, 0, 0, 0, 0, 0 })

/* Information passed to and from the parser.  */
typedef struct
{
  /* The input string remaining to be parsed. */
  const char *input;

  /* N, if this is the Nth Tuesday.  */
  long int day_ordinal;

  /* Day of week; Sunday is 0.  */
  int day_number;

  /* tm_isdst flag for the local zone.  */
  int local_isdst;

  /* Time zone, in minutes east of UTC.  */
  long int time_zone;

  /* Style used for time.  */
  int meridian;

  /* Gregorian year, month, day, hour, minutes, seconds, and nanoseconds.  */
  textint year;
  long int month;
  long int day;
  long int hour;
  long int minutes;
  struct timespec seconds; /* includes nanoseconds */

  /* Relative year, month, day, hour, minutes, seconds, and nanoseconds.  */
  relative_time rel;

  /* Presence or counts of nonterminals of various flavors parsed so far.  */
  bool timespec_seen;
  bool rels_seen;
  size_t dates_seen;
  size_t days_seen;
  size_t local_zones_seen;
  size_t dsts_seen;
  size_t times_seen;
  size_t zones_seen;

  /* Table of local time zone abbreviations, terminated by a null entry.  */
  table local_time_zone_table[3];
} parser_control;

union YYSTYPE;
static int yylex (union YYSTYPE *, parser_control *);
static int yyerror (parser_control const *, char const *);
static long int time_zone_hhmm (parser_control *, textint, long int);

/* Extract into *PC any date and time info from a string of digits
   of the form e.g., YYYYMMDD, YYMMDD, HHMM, HH (and sometimes YYY,
   YYYY, ...).  */
static void
digits_to_date_time (parser_control *pc, textint text_int)
{
  if (pc->dates_seen && ! pc->year.digits
      && ! pc->rels_seen && (pc->times_seen || 2 < text_int.digits))
    pc->year = text_int;
  else
    {
      if (4 < text_int.digits)
        {
          pc->dates_seen++;
          pc->day = text_int.value % 100;
          pc->month = (text_int.value / 100) % 100;
          pc->year.value = text_int.value / 10000;
          pc->year.digits = text_int.digits - 4;
        }
      else
        {
          pc->times_seen++;
          if (text_int.digits <= 2)
            {
              pc->hour = text_int.value;
              pc->minutes = 0;
            }
          else
            {
              pc->hour = text_int.value / 100;
              pc->minutes = text_int.value % 100;
            }
          pc->seconds.tv_sec = 0;
          pc->seconds.tv_nsec = 0;
          pc->meridian = MER24;
        }
    }
}

/* Increment PC->rel by FACTOR * REL (FACTOR is 1 or -1).  */
static void
apply_relative_time (parser_control *pc, relative_time rel, int factor)
{
  pc->rel.ns += factor * rel.ns;
  pc->rel.seconds += factor * rel.seconds;
  pc->rel.minutes += factor * rel.minutes;
  pc->rel.hour += factor * rel.hour;
  pc->rel.day += factor * rel.day;
  pc->rel.month += factor * rel.month;
  pc->rel.year += factor * rel.year;
  pc->rels_seen = true;
}

/* Set PC-> hour, minutes, seconds and nanoseconds members from arguments.  */
static void
set_hhmmss (parser_control *pc, long int hour, long int minutes,
            time_t sec, long int nsec)
{
  pc->hour = hour;
  pc->minutes = minutes;
  pc->seconds.tv_sec = sec;
  pc->seconds.tv_nsec = nsec;
}


#line 365 "src/ostree/parse-datetime.c" /* yacc.c:339  */

# ifndef YY_NULLPTR
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULLPTR nullptr
#  else
#   define YY_NULLPTR 0
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 0
#endif


/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    tAGO = 258,
    tDST = 259,
    tYEAR_UNIT = 260,
    tMONTH_UNIT = 261,
    tHOUR_UNIT = 262,
    tMINUTE_UNIT = 263,
    tSEC_UNIT = 264,
    tDAY_UNIT = 265,
    tDAY_SHIFT = 266,
    tDAY = 267,
    tDAYZONE = 268,
    tLOCAL_ZONE = 269,
    tMERIDIAN = 270,
    tMONTH = 271,
    tORDINAL = 272,
    tZONE = 273,
    tSNUMBER = 274,
    tUNUMBER = 275,
    tSDECIMAL_NUMBER = 276,
    tUDECIMAL_NUMBER = 277
  };
#endif
/* Tokens.  */
#define tAGO 258
#define tDST 259
#define tYEAR_UNIT 260
#define tMONTH_UNIT 261
#define tHOUR_UNIT 262
#define tMINUTE_UNIT 263
#define tSEC_UNIT 264
#define tDAY_UNIT 265
#define tDAY_SHIFT 266
#define tDAY 267
#define tDAYZONE 268
#define tLOCAL_ZONE 269
#define tMERIDIAN 270
#define tMONTH 271
#define tORDINAL 272
#define tZONE 273
#define tSNUMBER 274
#define tUNUMBER 275
#define tSDECIMAL_NUMBER 276
#define tUDECIMAL_NUMBER 277

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED

union YYSTYPE
{
#line 310 "src/ostree/parse-datetime.y" /* yacc.c:355  */

  long int intval;
  textint textintval;
  struct timespec timespec;
  relative_time rel;

#line 453 "src/ostree/parse-datetime.c" /* yacc.c:355  */
};

typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif



int yyparse (parser_control *pc);



/* Copy the second part of user declarations.  */

#line 469 "src/ostree/parse-datetime.c" /* yacc.c:358  */

#ifdef short
# undef short
#endif

#ifdef YYTYPE_UINT8
typedef YYTYPE_UINT8 yytype_uint8;
#else
typedef unsigned char yytype_uint8;
#endif

#ifdef YYTYPE_INT8
typedef YYTYPE_INT8 yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef YYTYPE_UINT16
typedef YYTYPE_UINT16 yytype_uint16;
#else
typedef unsigned short int yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short int yytype_int16;
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif ! defined YYSIZE_T
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned int
# endif
#endif

#define YYSIZE_MAXIMUM ((YYSIZE_T) -1)

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif

#ifndef YY_ATTRIBUTE
# if (defined __GNUC__                                               \
      && (2 < __GNUC__ || (__GNUC__ == 2 && 96 <= __GNUC_MINOR__)))  \
     || defined __SUNPRO_C && 0x5110 <= __SUNPRO_C
#  define YY_ATTRIBUTE(Spec) __attribute__(Spec)
# else
#  define YY_ATTRIBUTE(Spec) /* empty */
# endif
#endif

#ifndef YY_ATTRIBUTE_PURE
# define YY_ATTRIBUTE_PURE   YY_ATTRIBUTE ((__pure__))
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# define YY_ATTRIBUTE_UNUSED YY_ATTRIBUTE ((__unused__))
#endif

#if !defined _Noreturn \
     && (!defined __STDC_VERSION__ || __STDC_VERSION__ < 201112)
# if defined _MSC_VER && 1200 <= _MSC_VER
#  define _Noreturn __declspec (noreturn)
# else
#  define _Noreturn YY_ATTRIBUTE ((__noreturn__))
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN \
    _Pragma ("GCC diagnostic push") \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")\
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif


#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yytype_int16 yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (yytype_int16) + sizeof (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYSIZE_T yynewbytes;                                            \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / sizeof (*yyptr);                          \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, (Count) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYSIZE_T yyi;                         \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  12
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   112

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  28
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  26
/* YYNRULES -- Number of rules.  */
#define YYNRULES  91
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  114

/* YYTRANSLATE[YYX] -- Symbol number corresponding to YYX as returned
   by yylex, with out-of-bounds checking.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   277

#define YYTRANSLATE(YYX)                                                \
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, without out-of-bounds checking.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,    26,     2,     2,    27,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,    25,     2,
       2,     2,     2,     2,    23,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,    24,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   337,   337,   338,   342,   349,   351,   355,   357,   359,
     361,   363,   365,   367,   368,   369,   373,   377,   381,   386,
     391,   396,   400,   405,   410,   417,   419,   423,   431,   436,
     446,   448,   450,   453,   456,   458,   460,   465,   470,   475,
     480,   488,   493,   513,   521,   529,   534,   540,   545,   551,
     555,   565,   567,   569,   574,   576,   578,   580,   582,   584,
     586,   588,   590,   592,   594,   596,   598,   600,   602,   604,
     606,   608,   610,   612,   614,   618,   620,   622,   624,   626,
     628,   633,   637,   637,   640,   641,   646,   647,   652,   657,
     668,   669
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 0
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "tAGO", "tDST", "tYEAR_UNIT",
  "tMONTH_UNIT", "tHOUR_UNIT", "tMINUTE_UNIT", "tSEC_UNIT", "tDAY_UNIT",
  "tDAY_SHIFT", "tDAY", "tDAYZONE", "tLOCAL_ZONE", "tMERIDIAN", "tMONTH",
  "tORDINAL", "tZONE", "tSNUMBER", "tUNUMBER", "tSDECIMAL_NUMBER",
  "tUDECIMAL_NUMBER", "'@'", "'T'", "':'", "','", "'/'", "$accept", "spec",
  "timespec", "items", "item", "datetime", "iso_8601_datetime", "time",
  "iso_8601_time", "o_zone_offset", "zone_offset", "local_zone", "zone",
  "day", "date", "iso_8601_date", "rel", "relunit", "relunit_snumber",
  "dayshift", "seconds", "signed_seconds", "unsigned_seconds", "number",
  "hybrid", "o_colon_minutes", YY_NULLPTR
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_uint16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,    64,    84,    58,    44,    47
};
# endif

#define YYPACT_NINF -93

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-93)))

#define YYTABLE_NINF -1

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int8 yypact[] =
{
      38,    27,    77,   -93,    46,   -93,   -93,   -93,   -93,   -93,
     -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,
      62,   -93,    82,    -3,    66,     3,    74,    -4,    83,    84,
      75,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,
      71,   -93,    93,   -93,   -93,   -93,   -93,   -93,   -93,    78,
      72,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,    25,
     -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,
     -93,   -93,   -93,   -93,   -93,    21,    19,    79,    80,   -93,
     -93,   -93,   -93,   -93,    81,   -93,   -93,    85,    86,   -93,
     -93,   -93,   -93,   -93,    -6,    76,    17,   -93,   -93,   -93,
     -93,    87,    69,   -93,   -93,    88,    89,    -1,   -93,    18,
     -93,   -93,    69,    91
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       5,     0,     0,     2,     3,    85,    87,    84,    86,     4,
      82,    83,     1,    56,    59,    65,    68,    73,    62,    81,
      37,    35,    28,     0,     0,    30,     0,    88,     0,     0,
      31,     6,     7,    16,     8,    21,     9,    10,    12,    11,
      49,    13,    52,    74,    53,    14,    15,    38,    29,     0,
      45,    54,    57,    63,    66,    69,    60,    39,    36,    90,
      32,    75,    76,    78,    79,    80,    77,    55,    58,    64,
      67,    70,    61,    40,    18,    47,    90,     0,     0,    22,
      89,    71,    72,    33,     0,    51,    44,     0,     0,    34,
      43,    48,    50,    27,    25,    41,     0,    17,    46,    91,
      19,    90,     0,    23,    26,     0,     0,    25,    42,    25,
      20,    24,     0,    25
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -93,   -93,   -93,   -93,   -93,   -93,   -93,   -93,    20,   -68,
     -27,   -93,   -93,   -93,   -93,   -93,   -93,   -93,    60,   -93,
     -93,   -93,   -92,   -93,   -93,    43
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
      -1,     2,     3,     4,    31,    32,    33,    34,    35,   103,
     104,    36,    37,    38,    39,    40,    41,    42,    43,    44,
       9,    10,    11,    45,    46,    93
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint8 yytable[] =
{
      79,    67,    68,    69,    70,    71,    72,    58,    73,   100,
     107,    74,    75,   101,   110,    76,    49,    50,   101,   102,
     113,    77,    59,    78,    61,    62,    63,    64,    65,    66,
      61,    62,    63,    64,    65,    66,   101,   101,    92,   111,
      90,    91,   106,   112,    88,   111,     5,     6,     7,     8,
      88,    13,    14,    15,    16,    17,    18,    19,    20,    21,
      22,     1,    23,    24,    25,    26,    27,    28,    29,    79,
      30,    51,    52,    53,    54,    55,    56,    12,    57,    61,
      62,    63,    64,    65,    66,    60,    48,    80,    47,     6,
      83,     8,    81,    82,    26,    84,    85,    86,    87,    94,
      95,    96,    89,   105,    97,    98,    99,     0,   108,   109,
     101,     0,    88
};

static const yytype_int8 yycheck[] =
{
      27,     5,     6,     7,     8,     9,    10,     4,    12,    15,
     102,    15,    16,    19,    15,    19,    19,    20,    19,    25,
     112,    25,    19,    27,     5,     6,     7,     8,     9,    10,
       5,     6,     7,     8,     9,    10,    19,    19,    19,   107,
      19,    20,    25,    25,    25,   113,    19,    20,    21,    22,
      25,     5,     6,     7,     8,     9,    10,    11,    12,    13,
      14,    23,    16,    17,    18,    19,    20,    21,    22,    96,
      24,     5,     6,     7,     8,     9,    10,     0,    12,     5,
       6,     7,     8,     9,    10,    25,     4,    27,    26,    20,
      30,    22,     9,     9,    19,    24,     3,    19,    26,    20,
      20,    20,    59,    27,    84,    20,    20,    -1,    20,    20,
      19,    -1,    25
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,    23,    29,    30,    31,    19,    20,    21,    22,    48,
      49,    50,     0,     5,     6,     7,     8,     9,    10,    11,
      12,    13,    14,    16,    17,    18,    19,    20,    21,    22,
      24,    32,    33,    34,    35,    36,    39,    40,    41,    42,
      43,    44,    45,    46,    47,    51,    52,    26,     4,    19,
      20,     5,     6,     7,     8,     9,    10,    12,     4,    19,
      46,     5,     6,     7,     8,     9,    10,     5,     6,     7,
       8,     9,    10,    12,    15,    16,    19,    25,    27,    38,
      46,     9,     9,    46,    24,     3,    19,    26,    25,    53,
      19,    20,    19,    53,    20,    20,    20,    36,    20,    20,
      15,    19,    25,    37,    38,    27,    25,    50,    20,    20,
      15,    37,    25,    50
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,    28,    29,    29,    30,    31,    31,    32,    32,    32,
      32,    32,    32,    32,    32,    32,    33,    34,    35,    35,
      35,    35,    36,    36,    36,    37,    37,    38,    39,    39,
      40,    40,    40,    40,    40,    40,    40,    41,    41,    41,
      41,    42,    42,    42,    42,    42,    42,    42,    42,    42,
      43,    44,    44,    44,    45,    45,    45,    45,    45,    45,
      45,    45,    45,    45,    45,    45,    45,    45,    45,    45,
      45,    45,    45,    45,    45,    46,    46,    46,    46,    46,
      46,    47,    48,    48,    49,    49,    50,    50,    51,    52,
      53,    53
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     1,     1,     2,     0,     2,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     3,     2,     4,
       6,     1,     2,     4,     6,     0,     1,     2,     1,     2,
       1,     1,     2,     2,     3,     1,     2,     1,     2,     2,
       2,     3,     5,     3,     3,     2,     4,     2,     3,     1,
       3,     2,     1,     1,     2,     2,     1,     2,     2,     1,
       2,     2,     1,     2,     2,     1,     2,     2,     1,     2,
       2,     2,     2,     1,     1,     2,     2,     2,     2,     2,
       2,     1,     1,     1,     1,     1,     1,     1,     1,     2,
       0,     2
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                  \
do                                                              \
  if (yychar == YYEMPTY)                                        \
    {                                                           \
      yychar = (Token);                                         \
      yylval = (Value);                                         \
      YYPOPSTACK (yylen);                                       \
      yystate = *yyssp;                                         \
      goto yybackup;                                            \
    }                                                           \
  else                                                          \
    {                                                           \
      yyerror (pc, YY_("syntax error: cannot back up")); \
      YYERROR;                                                  \
    }                                                           \
while (0)

/* Error token number */
#define YYTERROR        1
#define YYERRCODE       256



/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)

/* This macro is provided for backward compatibility. */
#ifndef YY_LOCATION_PRINT
# define YY_LOCATION_PRINT(File, Loc) ((void) 0)
#endif


# define YY_SYMBOL_PRINT(Title, Type, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Type, Value, pc); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*----------------------------------------.
| Print this symbol's value on YYOUTPUT.  |
`----------------------------------------*/

static void
yy_symbol_value_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep, parser_control *pc)
{
  FILE *yyo = yyoutput;
  YYUSE (yyo);
  YYUSE (pc);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# endif
  YYUSE (yytype);
}


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

static void
yy_symbol_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep, parser_control *pc)
{
  YYFPRINTF (yyoutput, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  yy_symbol_value_print (yyoutput, yytype, yyvaluep, pc);
  YYFPRINTF (yyoutput, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yytype_int16 *yybottom, yytype_int16 *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yytype_int16 *yyssp, YYSTYPE *yyvsp, int yyrule, parser_control *pc)
{
  unsigned long int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %lu):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       yystos[yyssp[yyi + 1 - yynrhs]],
                       &(yyvsp[(yyi + 1) - (yynrhs)])
                                              , pc);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule, pc); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
static YYSIZE_T
yystrlen (const char *yystr)
{
  YYSIZE_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYSIZE_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYSIZE_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            /* Fall through.  */
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (! yyres)
    return yystrlen (yystr);

  return yystpcpy (yyres, yystr) - yyres;
}
# endif

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return 1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return 2 if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYSIZE_T *yymsg_alloc, char **yymsg,
                yytype_int16 *yyssp, int yytoken)
{
  YYSIZE_T yysize0 = yytnamerr (YY_NULLPTR, yytname[yytoken]);
  YYSIZE_T yysize = yysize0;
  enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat. */
  char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
  /* Number of reported tokens (one for the "unexpected", one per
     "expected"). */
  int yycount = 0;

  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yytoken != YYEMPTY)
    {
      int yyn = yypact[*yyssp];
      yyarg[yycount++] = yytname[yytoken];
      if (!yypact_value_is_default (yyn))
        {
          /* Start YYX at -YYN if negative to avoid negative indexes in
             YYCHECK.  In other words, skip the first -YYN actions for
             this state because they are default actions.  */
          int yyxbegin = yyn < 0 ? -yyn : 0;
          /* Stay within bounds of both yycheck and yytname.  */
          int yychecklim = YYLAST - yyn + 1;
          int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
          int yyx;

          for (yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR
                && !yytable_value_is_error (yytable[yyx + yyn]))
              {
                if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                  {
                    yycount = 1;
                    yysize = yysize0;
                    break;
                  }
                yyarg[yycount++] = yytname[yyx];
                {
                  YYSIZE_T yysize1 = yysize + yytnamerr (YY_NULLPTR, yytname[yyx]);
                  if (! (yysize <= yysize1
                         && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
                    return 2;
                  yysize = yysize1;
                }
              }
        }
    }

  switch (yycount)
    {
# define YYCASE_(N, S)                      \
      case N:                               \
        yyformat = S;                       \
      break
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
# undef YYCASE_
    }

  {
    YYSIZE_T yysize1 = yysize + yystrlen (yyformat);
    if (! (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
      return 2;
    yysize = yysize1;
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return 1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yyarg[yyi++]);
          yyformat += 2;
        }
      else
        {
          yyp++;
          yyformat++;
        }
  }
  return 0;
}
#endif /* YYERROR_VERBOSE */

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep, parser_control *pc)
{
  YYUSE (yyvaluep);
  YYUSE (pc);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}




/*----------.
| yyparse.  |
`----------*/

int
yyparse (parser_control *pc)
{
/* The lookahead symbol.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

    /* Number of syntax errors so far.  */
    int yynerrs;

    int yystate;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus;

    /* The stacks and their tools:
       'yyss': related to states.
       'yyvs': related to semantic values.

       Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* The state stack.  */
    yytype_int16 yyssa[YYINITDEPTH];
    yytype_int16 *yyss;
    yytype_int16 *yyssp;

    /* The semantic value stack.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs;
    YYSTYPE *yyvsp;

    YYSIZE_T yystacksize;

  int yyn;
  int yyresult;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;

#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYSIZE_T yymsg_alloc = sizeof yymsgbuf;
#endif

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  yyssp = yyss = yyssa;
  yyvsp = yyvs = yyvsa;
  yystacksize = YYINITDEPTH;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY; /* Cause a token to be read.  */
  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        YYSTYPE *yyvs1 = yyvs;
        yytype_int16 *yyss1 = yyss;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * sizeof (*yyssp),
                    &yyvs1, yysize * sizeof (*yyvsp),
                    &yystacksize);

        yyss = yyss1;
        yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyexhaustedlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yytype_int16 *yyss1 = yyss;
        union yyalloc *yyptr =
          (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
                  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = yylex (&yylval, pc);
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);

  /* Discard the shifted token.  */
  yychar = YYEMPTY;

  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
        case 4:
#line 343 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->seconds = (yyvsp[0].timespec);
        pc->timespec_seen = true;
      }
#line 1641 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 7:
#line 356 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->times_seen++; pc->dates_seen++; }
#line 1647 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 8:
#line 358 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->times_seen++; }
#line 1653 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 9:
#line 360 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->local_zones_seen++; }
#line 1659 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 10:
#line 362 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->zones_seen++; }
#line 1665 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 11:
#line 364 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->dates_seen++; }
#line 1671 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 12:
#line 366 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->days_seen++; }
#line 1677 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 18:
#line 382 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-1].textintval).value, 0, 0, 0);
        pc->meridian = (yyvsp[0].intval);
      }
#line 1686 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 19:
#line 387 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-3].textintval).value, (yyvsp[-1].textintval).value, 0, 0);
        pc->meridian = (yyvsp[0].intval);
      }
#line 1695 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 20:
#line 392 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-5].textintval).value, (yyvsp[-3].textintval).value, (yyvsp[-1].timespec).tv_sec, (yyvsp[-1].timespec).tv_nsec);
        pc->meridian = (yyvsp[0].intval);
      }
#line 1704 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 22:
#line 401 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-1].textintval).value, 0, 0, 0);
        pc->meridian = MER24;
      }
#line 1713 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 23:
#line 406 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-3].textintval).value, (yyvsp[-1].textintval).value, 0, 0);
        pc->meridian = MER24;
      }
#line 1722 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 24:
#line 411 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        set_hhmmss (pc, (yyvsp[-5].textintval).value, (yyvsp[-3].textintval).value, (yyvsp[-1].timespec).tv_sec, (yyvsp[-1].timespec).tv_nsec);
        pc->meridian = MER24;
      }
#line 1731 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 27:
#line 424 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->zones_seen++;
        pc->time_zone = time_zone_hhmm (pc, (yyvsp[-1].textintval), (yyvsp[0].intval));
      }
#line 1740 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 28:
#line 432 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->local_isdst = (yyvsp[0].intval);
        pc->dsts_seen += (0 < (yyvsp[0].intval));
      }
#line 1749 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 29:
#line 437 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->local_isdst = 1;
        pc->dsts_seen += (0 < (yyvsp[-1].intval)) + 1;
      }
#line 1758 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 30:
#line 447 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = (yyvsp[0].intval); }
#line 1764 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 31:
#line 449 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = HOUR(7); }
#line 1770 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 32:
#line 451 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = (yyvsp[-1].intval);
        apply_relative_time (pc, (yyvsp[0].rel), 1); }
#line 1777 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 33:
#line 454 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = HOUR(7);
        apply_relative_time (pc, (yyvsp[0].rel), 1); }
#line 1784 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 34:
#line 457 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = (yyvsp[-2].intval) + time_zone_hhmm (pc, (yyvsp[-1].textintval), (yyvsp[0].intval)); }
#line 1790 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 35:
#line 459 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = (yyvsp[0].intval) + 60; }
#line 1796 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 36:
#line 461 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { pc->time_zone = (yyvsp[-1].intval) + 60; }
#line 1802 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 37:
#line 466 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day_ordinal = 0;
        pc->day_number = (yyvsp[0].intval);
      }
#line 1811 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 38:
#line 471 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day_ordinal = 0;
        pc->day_number = (yyvsp[-1].intval);
      }
#line 1820 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 39:
#line 476 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day_ordinal = (yyvsp[-1].intval);
        pc->day_number = (yyvsp[0].intval);
      }
#line 1829 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 40:
#line 481 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day_ordinal = (yyvsp[-1].textintval).value;
        pc->day_number = (yyvsp[0].intval);
      }
#line 1838 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 41:
#line 489 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->month = (yyvsp[-2].textintval).value;
        pc->day = (yyvsp[0].textintval).value;
      }
#line 1847 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 42:
#line 494 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        /* Interpret as YYYY/MM/DD if the first value has 4 or more digits,
           otherwise as MM/DD/YY.
           The goal in recognizing YYYY/MM/DD is solely to support legacy
           machine-generated dates like those in an RCS log listing.  If
           you want portability, use the ISO 8601 format.  */
        if (4 <= (yyvsp[-4].textintval).digits)
          {
            pc->year = (yyvsp[-4].textintval);
            pc->month = (yyvsp[-2].textintval).value;
            pc->day = (yyvsp[0].textintval).value;
          }
        else
          {
            pc->month = (yyvsp[-4].textintval).value;
            pc->day = (yyvsp[-2].textintval).value;
            pc->year = (yyvsp[0].textintval);
          }
      }
#line 1871 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 43:
#line 514 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        /* e.g. 17-JUN-1992.  */
        pc->day = (yyvsp[-2].textintval).value;
        pc->month = (yyvsp[-1].intval);
        pc->year.value = -(yyvsp[0].textintval).value;
        pc->year.digits = (yyvsp[0].textintval).digits;
      }
#line 1883 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 44:
#line 522 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        /* e.g. JUN-17-1992.  */
        pc->month = (yyvsp[-2].intval);
        pc->day = -(yyvsp[-1].textintval).value;
        pc->year.value = -(yyvsp[0].textintval).value;
        pc->year.digits = (yyvsp[0].textintval).digits;
      }
#line 1895 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 45:
#line 530 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->month = (yyvsp[-1].intval);
        pc->day = (yyvsp[0].textintval).value;
      }
#line 1904 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 46:
#line 535 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->month = (yyvsp[-3].intval);
        pc->day = (yyvsp[-2].textintval).value;
        pc->year = (yyvsp[0].textintval);
      }
#line 1914 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 47:
#line 541 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day = (yyvsp[-1].textintval).value;
        pc->month = (yyvsp[0].intval);
      }
#line 1923 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 48:
#line 546 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        pc->day = (yyvsp[-2].textintval).value;
        pc->month = (yyvsp[-1].intval);
        pc->year = (yyvsp[0].textintval);
      }
#line 1933 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 50:
#line 556 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        /* ISO 8601 format.  YYYY-MM-DD.  */
        pc->year = (yyvsp[-2].textintval);
        pc->month = -(yyvsp[-1].textintval).value;
        pc->day = -(yyvsp[0].textintval).value;
      }
#line 1944 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 51:
#line 566 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { apply_relative_time (pc, (yyvsp[-1].rel), (yyvsp[0].intval)); }
#line 1950 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 52:
#line 568 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { apply_relative_time (pc, (yyvsp[0].rel), 1); }
#line 1956 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 53:
#line 570 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { apply_relative_time (pc, (yyvsp[0].rel), 1); }
#line 1962 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 54:
#line 575 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).year = (yyvsp[-1].intval); }
#line 1968 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 55:
#line 577 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).year = (yyvsp[-1].textintval).value; }
#line 1974 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 56:
#line 579 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).year = 1; }
#line 1980 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 57:
#line 581 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).month = (yyvsp[-1].intval); }
#line 1986 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 58:
#line 583 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).month = (yyvsp[-1].textintval).value; }
#line 1992 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 59:
#line 585 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).month = 1; }
#line 1998 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 60:
#line 587 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).day = (yyvsp[-1].intval) * (yyvsp[0].intval); }
#line 2004 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 61:
#line 589 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).day = (yyvsp[-1].textintval).value * (yyvsp[0].intval); }
#line 2010 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 62:
#line 591 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).day = (yyvsp[0].intval); }
#line 2016 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 63:
#line 593 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).hour = (yyvsp[-1].intval); }
#line 2022 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 64:
#line 595 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).hour = (yyvsp[-1].textintval).value; }
#line 2028 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 65:
#line 597 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).hour = 1; }
#line 2034 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 66:
#line 599 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).minutes = (yyvsp[-1].intval); }
#line 2040 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 67:
#line 601 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).minutes = (yyvsp[-1].textintval).value; }
#line 2046 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 68:
#line 603 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).minutes = 1; }
#line 2052 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 69:
#line 605 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = (yyvsp[-1].intval); }
#line 2058 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 70:
#line 607 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = (yyvsp[-1].textintval).value; }
#line 2064 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 71:
#line 609 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = (yyvsp[-1].timespec).tv_sec; (yyval.rel).ns = (yyvsp[-1].timespec).tv_nsec; }
#line 2070 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 72:
#line 611 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = (yyvsp[-1].timespec).tv_sec; (yyval.rel).ns = (yyvsp[-1].timespec).tv_nsec; }
#line 2076 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 73:
#line 613 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = 1; }
#line 2082 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 75:
#line 619 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).year = (yyvsp[-1].textintval).value; }
#line 2088 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 76:
#line 621 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).month = (yyvsp[-1].textintval).value; }
#line 2094 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 77:
#line 623 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).day = (yyvsp[-1].textintval).value * (yyvsp[0].intval); }
#line 2100 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 78:
#line 625 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).hour = (yyvsp[-1].textintval).value; }
#line 2106 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 79:
#line 627 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).minutes = (yyvsp[-1].textintval).value; }
#line 2112 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 80:
#line 629 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).seconds = (yyvsp[-1].textintval).value; }
#line 2118 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 81:
#line 634 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.rel) = RELATIVE_TIME_0; (yyval.rel).day = (yyvsp[0].intval); }
#line 2124 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 85:
#line 642 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.timespec).tv_sec = (yyvsp[0].textintval).value; (yyval.timespec).tv_nsec = 0; }
#line 2130 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 87:
#line 648 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.timespec).tv_sec = (yyvsp[0].textintval).value; (yyval.timespec).tv_nsec = 0; }
#line 2136 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 88:
#line 653 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { digits_to_date_time (pc, (yyvsp[0].textintval)); }
#line 2142 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 89:
#line 658 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    {
        /* Hybrid all-digit and relative offset, so that we accept e.g.,
           "YYYYMMDD +N days" as well as "YYYYMMDD N days".  */
        digits_to_date_time (pc, (yyvsp[-1].textintval));
        apply_relative_time (pc, (yyvsp[0].rel), 1);
      }
#line 2153 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 90:
#line 668 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.intval) = -1; }
#line 2159 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;

  case 91:
#line 670 "src/ostree/parse-datetime.y" /* yacc.c:1646  */
    { (yyval.intval) = (yyvsp[0].textintval).value; }
#line 2165 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
    break;


#line 2169 "src/ostree/parse-datetime.c" /* yacc.c:1646  */
      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYEMPTY : YYTRANSLATE (yychar);

  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (pc, YY_("syntax error"));
#else
# define YYSYNTAX_ERROR yysyntax_error (&yymsg_alloc, &yymsg, \
                                        yyssp, yytoken)
      {
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = YYSYNTAX_ERROR;
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == 1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = (char *) YYSTACK_ALLOC (yymsg_alloc);
            if (!yymsg)
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = 2;
              }
            else
              {
                yysyntax_error_status = YYSYNTAX_ERROR;
                yymsgp = yymsg;
              }
          }
        yyerror (pc, yymsgp);
        if (yysyntax_error_status == 2)
          goto yyexhaustedlab;
      }
# undef YYSYNTAX_ERROR
#endif
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, pc);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:

  /* Pacify compilers like GCC when the user code never invokes
     YYERROR and the label yyerrorlab therefore never appears in user
     code.  */
  if (/*CONSTCOND*/ 0)
     goto yyerrorlab;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYTERROR;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  yystos[yystate], yyvsp, pc);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#if !defined yyoverflow || YYERROR_VERBOSE
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (pc, YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, pc);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  yystos[*yyssp], yyvsp, pc);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  return yyresult;
}
#line 673 "src/ostree/parse-datetime.y" /* yacc.c:1906  */


static table const meridian_table[] =
{
  { "AM",   tMERIDIAN, MERam },
  { "A.M.", tMERIDIAN, MERam },
  { "PM",   tMERIDIAN, MERpm },
  { "P.M.", tMERIDIAN, MERpm },
  { NULL, 0, 0 }
};

static table const dst_table[] =
{
  { "DST", tDST, 0 }
};

static table const month_and_day_table[] =
{
  { "JANUARY",  tMONTH,  1 },
  { "FEBRUARY", tMONTH,  2 },
  { "MARCH",    tMONTH,  3 },
  { "APRIL",    tMONTH,  4 },
  { "MAY",      tMONTH,  5 },
  { "JUNE",     tMONTH,  6 },
  { "JULY",     tMONTH,  7 },
  { "AUGUST",   tMONTH,  8 },
  { "SEPTEMBER",tMONTH,  9 },
  { "SEPT",     tMONTH,  9 },
  { "OCTOBER",  tMONTH, 10 },
  { "NOVEMBER", tMONTH, 11 },
  { "DECEMBER", tMONTH, 12 },
  { "SUNDAY",   tDAY,    0 },
  { "MONDAY",   tDAY,    1 },
  { "TUESDAY",  tDAY,    2 },
  { "TUES",     tDAY,    2 },
  { "WEDNESDAY",tDAY,    3 },
  { "WEDNES",   tDAY,    3 },
  { "THURSDAY", tDAY,    4 },
  { "THUR",     tDAY,    4 },
  { "THURS",    tDAY,    4 },
  { "FRIDAY",   tDAY,    5 },
  { "SATURDAY", tDAY,    6 },
  { NULL, 0, 0 }
};

static table const time_units_table[] =
{
  { "YEAR",     tYEAR_UNIT,      1 },
  { "MONTH",    tMONTH_UNIT,     1 },
  { "FORTNIGHT",tDAY_UNIT,      14 },
  { "WEEK",     tDAY_UNIT,       7 },
  { "DAY",      tDAY_UNIT,       1 },
  { "HOUR",     tHOUR_UNIT,      1 },
  { "MINUTE",   tMINUTE_UNIT,    1 },
  { "MIN",      tMINUTE_UNIT,    1 },
  { "SECOND",   tSEC_UNIT,       1 },
  { "SEC",      tSEC_UNIT,       1 },
  { NULL, 0, 0 }
};

/* Assorted relative-time words. */
static table const relative_time_table[] =
{
  { "TOMORROW", tDAY_SHIFT,      1 },
  { "YESTERDAY",tDAY_SHIFT,     -1 },
  { "TODAY",    tDAY_SHIFT,      0 },
  { "NOW",      tDAY_SHIFT,      0 },
  { "LAST",     tORDINAL,       -1 },
  { "THIS",     tORDINAL,        0 },
  { "NEXT",     tORDINAL,        1 },
  { "FIRST",    tORDINAL,        1 },
/*{ "SECOND",   tORDINAL,        2 }, */
  { "THIRD",    tORDINAL,        3 },
  { "FOURTH",   tORDINAL,        4 },
  { "FIFTH",    tORDINAL,        5 },
  { "SIXTH",    tORDINAL,        6 },
  { "SEVENTH",  tORDINAL,        7 },
  { "EIGHTH",   tORDINAL,        8 },
  { "NINTH",    tORDINAL,        9 },
  { "TENTH",    tORDINAL,       10 },
  { "ELEVENTH", tORDINAL,       11 },
  { "TWELFTH",  tORDINAL,       12 },
  { "AGO",      tAGO,           -1 },
  { "HENCE",    tAGO,            1 },
  { NULL, 0, 0 }
};

/* The universal time zone table.  These labels can be used even for
   time stamps that would not otherwise be valid, e.g., GMT time
   stamps in London during summer.  */
static table const universal_time_zone_table[] =
{
  { "GMT",      tZONE,     HOUR ( 0) }, /* Greenwich Mean */
  { "UT",       tZONE,     HOUR ( 0) }, /* Universal (Coordinated) */
  { "UTC",      tZONE,     HOUR ( 0) },
  { NULL, 0, 0 }
};

/* The time zone table.  This table is necessarily incomplete, as time
   zone abbreviations are ambiguous; e.g. Australians interpret "EST"
   as Eastern time in Australia, not as US Eastern Standard Time.
   You cannot rely on parse_datetime to handle arbitrary time zone
   abbreviations; use numeric abbreviations like "-0500" instead.  */
static table const time_zone_table[] =
{
  { "WET",      tZONE,     HOUR ( 0) }, /* Western European */
  { "WEST",     tDAYZONE,  HOUR ( 0) }, /* Western European Summer */
  { "BST",      tDAYZONE,  HOUR ( 0) }, /* British Summer */
  { "ART",      tZONE,    -HOUR ( 3) }, /* Argentina */
  { "BRT",      tZONE,    -HOUR ( 3) }, /* Brazil */
  { "BRST",     tDAYZONE, -HOUR ( 3) }, /* Brazil Summer */
  { "NST",      tZONE,   -(HOUR ( 3) + 30) },   /* Newfoundland Standard */
  { "NDT",      tDAYZONE,-(HOUR ( 3) + 30) },   /* Newfoundland Daylight */
  { "AST",      tZONE,    -HOUR ( 4) }, /* Atlantic Standard */
  { "ADT",      tDAYZONE, -HOUR ( 4) }, /* Atlantic Daylight */
  { "CLT",      tZONE,    -HOUR ( 4) }, /* Chile */
  { "CLST",     tDAYZONE, -HOUR ( 4) }, /* Chile Summer */
  { "EST",      tZONE,    -HOUR ( 5) }, /* Eastern Standard */
  { "EDT",      tDAYZONE, -HOUR ( 5) }, /* Eastern Daylight */
  { "CST",      tZONE,    -HOUR ( 6) }, /* Central Standard */
  { "CDT",      tDAYZONE, -HOUR ( 6) }, /* Central Daylight */
  { "MST",      tZONE,    -HOUR ( 7) }, /* Mountain Standard */
  { "MDT",      tDAYZONE, -HOUR ( 7) }, /* Mountain Daylight */
  { "PST",      tZONE,    -HOUR ( 8) }, /* Pacific Standard */
  { "PDT",      tDAYZONE, -HOUR ( 8) }, /* Pacific Daylight */
  { "AKST",     tZONE,    -HOUR ( 9) }, /* Alaska Standard */
  { "AKDT",     tDAYZONE, -HOUR ( 9) }, /* Alaska Daylight */
  { "HST",      tZONE,    -HOUR (10) }, /* Hawaii Standard */
  { "HAST",     tZONE,    -HOUR (10) }, /* Hawaii-Aleutian Standard */
  { "HADT",     tDAYZONE, -HOUR (10) }, /* Hawaii-Aleutian Daylight */
  { "SST",      tZONE,    -HOUR (12) }, /* Samoa Standard */
  { "WAT",      tZONE,     HOUR ( 1) }, /* West Africa */
  { "CET",      tZONE,     HOUR ( 1) }, /* Central European */
  { "CEST",     tDAYZONE,  HOUR ( 1) }, /* Central European Summer */
  { "MET",      tZONE,     HOUR ( 1) }, /* Middle European */
  { "MEZ",      tZONE,     HOUR ( 1) }, /* Middle European */
  { "MEST",     tDAYZONE,  HOUR ( 1) }, /* Middle European Summer */
  { "MESZ",     tDAYZONE,  HOUR ( 1) }, /* Middle European Summer */
  { "EET",      tZONE,     HOUR ( 2) }, /* Eastern European */
  { "EEST",     tDAYZONE,  HOUR ( 2) }, /* Eastern European Summer */
  { "CAT",      tZONE,     HOUR ( 2) }, /* Central Africa */
  { "SAST",     tZONE,     HOUR ( 2) }, /* South Africa Standard */
  { "EAT",      tZONE,     HOUR ( 3) }, /* East Africa */
  { "MSK",      tZONE,     HOUR ( 3) }, /* Moscow */
  { "MSD",      tDAYZONE,  HOUR ( 3) }, /* Moscow Daylight */
  { "IST",      tZONE,    (HOUR ( 5) + 30) },   /* India Standard */
  { "SGT",      tZONE,     HOUR ( 8) }, /* Singapore */
  { "KST",      tZONE,     HOUR ( 9) }, /* Korea Standard */
  { "JST",      tZONE,     HOUR ( 9) }, /* Japan Standard */
  { "GST",      tZONE,     HOUR (10) }, /* Guam Standard */
  { "NZST",     tZONE,     HOUR (12) }, /* New Zealand Standard */
  { "NZDT",     tDAYZONE,  HOUR (12) }, /* New Zealand Daylight */
  { NULL, 0, 0 }
};

/* Military time zone table.

   Note 'T' is a special case, as it is used as the separator in ISO
   8601 date and time of day representation. */
static table const military_table[] =
{
  { "A", tZONE, -HOUR ( 1) },
  { "B", tZONE, -HOUR ( 2) },
  { "C", tZONE, -HOUR ( 3) },
  { "D", tZONE, -HOUR ( 4) },
  { "E", tZONE, -HOUR ( 5) },
  { "F", tZONE, -HOUR ( 6) },
  { "G", tZONE, -HOUR ( 7) },
  { "H", tZONE, -HOUR ( 8) },
  { "I", tZONE, -HOUR ( 9) },
  { "K", tZONE, -HOUR (10) },
  { "L", tZONE, -HOUR (11) },
  { "M", tZONE, -HOUR (12) },
  { "N", tZONE,  HOUR ( 1) },
  { "O", tZONE,  HOUR ( 2) },
  { "P", tZONE,  HOUR ( 3) },
  { "Q", tZONE,  HOUR ( 4) },
  { "R", tZONE,  HOUR ( 5) },
  { "S", tZONE,  HOUR ( 6) },
  { "T", 'T',    0 },
  { "U", tZONE,  HOUR ( 8) },
  { "V", tZONE,  HOUR ( 9) },
  { "W", tZONE,  HOUR (10) },
  { "X", tZONE,  HOUR (11) },
  { "Y", tZONE,  HOUR (12) },
  { "Z", tZONE,  HOUR ( 0) },
  { NULL, 0, 0 }
};



/* Convert a time zone expressed as HH:MM into an integer count of
   minutes.  If MM is negative, then S is of the form HHMM and needs
   to be picked apart; otherwise, S is of the form HH.  As specified in
   http://www.opengroup.org/susv3xbd/xbd_chap08.html#tag_08_03, allow
   only valid TZ range, and consider first two digits as hours, if no
   minutes specified.  */

static long int
time_zone_hhmm (parser_control *pc, textint s, long int mm)
{
  long int n_minutes;

  /* If the length of S is 1 or 2 and no minutes are specified,
     interpret it as a number of hours.  */
  if (s.digits <= 2 && mm < 0)
    s.value *= 100;

  if (mm < 0)
    n_minutes = (s.value / 100) * 60 + s.value % 100;
  else
    n_minutes = s.value * 60 + (s.negative ? -mm : mm);

  /* If the absolute number of minutes is larger than 24 hours,
     arrange to reject it by incrementing pc->zones_seen.  Thus,
     we allow only values in the range UTC-24:00 to UTC+24:00.  */
  if (24 * 60 < labs (n_minutes))
    pc->zones_seen++;

  return n_minutes;
}

static int
to_hour (long int hours, int meridian)
{
  switch (meridian)
    {
    default: /* Pacify GCC.  */
    case MER24:
      return 0 <= hours && hours < 24 ? hours : -1;
    case MERam:
      return 0 < hours && hours < 12 ? hours : hours == 12 ? 0 : -1;
    case MERpm:
      return 0 < hours && hours < 12 ? hours + 12 : hours == 12 ? 12 : -1;
    }
}

static long int
to_year (textint textyear)
{
  long int year = textyear.value;

  if (year < 0)
    year = -year;

  /* XPG4 suggests that years 00-68 map to 2000-2068, and
     years 69-99 map to 1969-1999.  */
  else if (textyear.digits == 2)
    year += year < 69 ? 2000 : 1900;

  return year;
}

static table const *
lookup_zone (parser_control const *pc, char const *name)
{
  table const *tp;

  for (tp = universal_time_zone_table; tp->name; tp++)
    if (strcmp (name, tp->name) == 0)
      return tp;

  /* Try local zone abbreviations before those in time_zone_table, as
     the local ones are more likely to be right.  */
  for (tp = pc->local_time_zone_table; tp->name; tp++)
    if (strcmp (name, tp->name) == 0)
      return tp;

  for (tp = time_zone_table; tp->name; tp++)
    if (strcmp (name, tp->name) == 0)
      return tp;

  return NULL;
}

// #if ! HAVE_TM_GMTOFF
#if 1 // Always true for us
/* Yield the difference between *A and *B,
   measured in seconds, ignoring leap seconds.
   The body of this function is taken directly from the GNU C Library;
   see src/strftime.c.  */
static long int
tm_diff (struct tm const *a, struct tm const *b)
{
  /* Compute intervening leap days correctly even if year is negative.
     Take care to avoid int overflow in leap day calculations.  */
  int a4 = SHR (a->tm_year, 2) + SHR (TM_YEAR_BASE, 2) - ! (a->tm_year & 3);
  int b4 = SHR (b->tm_year, 2) + SHR (TM_YEAR_BASE, 2) - ! (b->tm_year & 3);
  int a100 = a4 / 25 - (a4 % 25 < 0);
  int b100 = b4 / 25 - (b4 % 25 < 0);
  int a400 = SHR (a100, 2);
  int b400 = SHR (b100, 2);
  int intervening_leap_days = (a4 - b4) - (a100 - b100) + (a400 - b400);
  long int ayear = a->tm_year;
  long int years = ayear - b->tm_year;
  long int days = (365 * years + intervening_leap_days
                   + (a->tm_yday - b->tm_yday));
  return (60 * (60 * (24 * days + (a->tm_hour - b->tm_hour))
                + (a->tm_min - b->tm_min))
          + (a->tm_sec - b->tm_sec));
}
#endif /* ! HAVE_TM_GMTOFF */

static table const *
lookup_word (parser_control const *pc, char *word)
{
  char *p;
  char *q;
  size_t wordlen;
  table const *tp;
  bool period_found;
  bool abbrev;

  /* Make it uppercase.  */
  for (p = word; *p; p++)
    {
      unsigned char ch = *p;
      *p = toupper (ch);
    }

  for (tp = meridian_table; tp->name; tp++)
    if (strcmp (word, tp->name) == 0)
      return tp;

  /* See if we have an abbreviation for a month. */
  wordlen = strlen (word);
  abbrev = wordlen == 3 || (wordlen == 4 && word[3] == '.');

  for (tp = month_and_day_table; tp->name; tp++)
    if ((abbrev ? strncmp (word, tp->name, 3) : strcmp (word, tp->name)) == 0)
      return tp;

  if ((tp = lookup_zone (pc, word)))
    return tp;

  if (strcmp (word, dst_table[0].name) == 0)
    return dst_table;

  for (tp = time_units_table; tp->name; tp++)
    if (strcmp (word, tp->name) == 0)
      return tp;

  /* Strip off any plural and try the units table again. */
  if (word[wordlen - 1] == 'S')
    {
      word[wordlen - 1] = '\0';
      for (tp = time_units_table; tp->name; tp++)
        if (strcmp (word, tp->name) == 0)
          return tp;
      word[wordlen - 1] = 'S';  /* For "this" in relative_time_table.  */
    }

  for (tp = relative_time_table; tp->name; tp++)
    if (strcmp (word, tp->name) == 0)
      return tp;

  /* Military time zones. */
  if (wordlen == 1)
    for (tp = military_table; tp->name; tp++)
      if (word[0] == tp->name[0])
        return tp;

  /* Drop out any periods and try the time zone table again. */
  for (period_found = false, p = q = word; (*p = *q); q++)
    if (*q == '.')
      period_found = true;
    else
      p++;
  if (period_found && (tp = lookup_zone (pc, word)))
    return tp;

  return NULL;
}

static int
yylex (union YYSTYPE *lvalp, parser_control *pc)
{
  unsigned char c;
  size_t count;

  for (;;)
    {
      while (c = *pc->input, isspace (c))
        pc->input++;

      if (ISDIGIT (c) || c == '-' || c == '+')
        {
          char const *p;
          int sign;
          unsigned long int value;
          if (c == '-' || c == '+')
            {
              sign = c == '-' ? -1 : 1;
              while (c = *++pc->input, isspace (c))
                continue;
              if (! ISDIGIT (c))
                /* skip the '-' sign */
                continue;
            }
          else
            sign = 0;
          p = pc->input;
          for (value = 0; ; value *= 10)
            {
              unsigned long int value1 = value + (c - '0');
              if (value1 < value)
                return '?';
              value = value1;
              c = *++p;
              if (! ISDIGIT (c))
                break;
              if (ULONG_MAX / 10 < value)
                return '?';
            }
          if ((c == '.' || c == ',') && ISDIGIT (p[1]))
            {
              time_t s;
              int ns;
              int digits;
              unsigned long int value1;

              /* Check for overflow when converting value to time_t.  */
              if (sign < 0)
                {
                  s = - value;
                  if (0 < s)
                    return '?';
                  value1 = -s;
                }
              else
                {
                  s = value;
                  if (s < 0)
                    return '?';
                  value1 = s;
                }
              if (value != value1)
                return '?';

              /* Accumulate fraction, to ns precision.  */
              p++;
              ns = *p++ - '0';
              for (digits = 2; digits <= LOG10_BILLION; digits++)
                {
                  ns *= 10;
                  if (ISDIGIT (*p))
                    ns += *p++ - '0';
                }

              /* Skip excess digits, truncating toward -Infinity.  */
              if (sign < 0)
                for (; ISDIGIT (*p); p++)
                  if (*p != '0')
                    {
                      ns++;
                      break;
                    }
              while (ISDIGIT (*p))
                p++;

              /* Adjust to the timespec convention, which is that
                 tv_nsec is always a positive offset even if tv_sec is
                 negative.  */
              if (sign < 0 && ns)
                {
                  s--;
                  if (! (s < 0))
                    return '?';
                  ns = BILLION - ns;
                }

              lvalp->timespec.tv_sec = s;
              lvalp->timespec.tv_nsec = ns;
              pc->input = p;
              return sign ? tSDECIMAL_NUMBER : tUDECIMAL_NUMBER;
            }
          else
            {
              lvalp->textintval.negative = sign < 0;
              if (sign < 0)
                {
                  lvalp->textintval.value = - value;
                  if (0 < lvalp->textintval.value)
                    return '?';
                }
              else
                {
                  lvalp->textintval.value = value;
                  if (lvalp->textintval.value < 0)
                    return '?';
                }
              lvalp->textintval.digits = p - pc->input;
              pc->input = p;
              return sign ? tSNUMBER : tUNUMBER;
            }
        }

      if (isalpha (c))
        {
          char buff[20];
          char *p = buff;
          table const *tp;

          do
            {
              if (p < buff + sizeof buff - 1)
                *p++ = c;
              c = *++pc->input;
            }
          while (isalpha (c) || c == '.');

          *p = '\0';
          tp = lookup_word (pc, buff);
          if (! tp)
            return '?';
          lvalp->intval = tp->value;
          return tp->type;
        }

      if (c != '(')
        return to_uchar (*pc->input++);

      count = 0;
      do
        {
          c = *pc->input++;
          if (c == '\0')
            return c;
          if (c == '(')
            count++;
          else if (c == ')')
            count--;
        }
      while (count != 0);
    }
}

/* Do nothing if the parser reports an error.  */
static int
yyerror (parser_control const *pc,
         char const *s)
{
  return 0;
}

/* If *TM0 is the old and *TM1 is the new value of a struct tm after
   passing it to mktime, return true if it's OK that mktime returned T.
   It's not OK if *TM0 has out-of-range members.  */

static bool
mktime_ok (struct tm const *tm0, struct tm const *tm1, time_t t)
{
  if (t == (time_t) -1)
    {
      /* Guard against falsely reporting an error when parsing a time
         stamp that happens to equal (time_t) -1, on a host that
         supports such a time stamp.  */
      tm1 = localtime (&t);
      if (!tm1)
        return false;
    }

  return ! ((tm0->tm_sec ^ tm1->tm_sec)
            | (tm0->tm_min ^ tm1->tm_min)
            | (tm0->tm_hour ^ tm1->tm_hour)
            | (tm0->tm_mday ^ tm1->tm_mday)
            | (tm0->tm_mon ^ tm1->tm_mon)
            | (tm0->tm_year ^ tm1->tm_year));
}

/* A reasonable upper bound for the size of ordinary TZ strings.
   Use heap allocation if TZ's length exceeds this.  */
enum { TZBUFSIZE = 100 };

/* Return a copy of TZ, stored in TZBUF if it fits, and heap-allocated
   otherwise.  */
static char *
get_tz (char tzbuf[TZBUFSIZE])
{
  char *tz = getenv ("TZ");
  if (tz)
    {
      size_t tzsize = strlen (tz) + 1;
      tz = (tzsize <= TZBUFSIZE
            ? memcpy (tzbuf, tz, tzsize)
            : xmemdup (tz, tzsize));
    }
  return tz;
}

/* Parse a date/time string, storing the resulting time value into *RESULT.
   The string itself is pointed to by P.  Return true if successful.
   P can be an incomplete or relative time specification; if so, use
   *NOW as the basis for the returned time.  */
bool
parse_datetime (struct timespec *result, char const *p,
                struct timespec const *now)
{
  time_t Start;
  long int Start_ns;
  struct tm const *tmp;
  struct tm tm = { 0, };
  struct tm tm0 = { 0, };
  parser_control pc;
  struct timespec gettime_buffer;
  unsigned char c;
  bool tz_was_altered = false;
  char *tz0 = NULL;
  char tz0buf[TZBUFSIZE];
  bool ok = true;

  if (! now)
    {
      gettime (&gettime_buffer);
      now = &gettime_buffer;
    }

  Start = now->tv_sec;
  Start_ns = now->tv_nsec;

  tmp = localtime (&now->tv_sec);
  if (! tmp)
    return false;

  while (c = *p, isspace (c))
    p++;

  if (strncmp (p, "TZ=\"", 4) == 0)
    {
      char const *tzbase = p + 4;
      size_t tzsize = 1;
      char const *s;

      for (s = tzbase; *s; s++, tzsize++)
        if (*s == '\\')
          {
            s++;
            if (! (*s == '\\' || *s == '"'))
              break;
          }
        else if (*s == '"')
          {
            char *z;
            char *tz1;
            char tz1buf[TZBUFSIZE];
            bool large_tz = TZBUFSIZE < tzsize;
            bool setenv_ok;
            tz0 = get_tz (tz0buf);
            z = tz1 = large_tz ? g_malloc (tzsize) : tz1buf;
            for (s = tzbase; *s != '"'; s++)
              *z++ = *(s += *s == '\\');
            *z = '\0';
            setenv_ok = setenv ("TZ", tz1, 1) == 0;
            if (large_tz)
              free (tz1);
            if (!setenv_ok)
              goto fail;
            tz_was_altered = true;

            p = s + 1;
            while (c = *p, isspace (c))
              p++;

            break;
          }
    }

  /* As documented, be careful to treat the empty string just like
     a date string of "0".  Without this, an empty string would be
     declared invalid when parsed during a DST transition.  */
  if (*p == '\0')
    p = "0";

  pc.input = p;
  pc.year.value = tmp->tm_year;
  pc.year.value += TM_YEAR_BASE;
  pc.year.digits = 0;
  pc.month = tmp->tm_mon + 1;
  pc.day = tmp->tm_mday;
  pc.hour = tmp->tm_hour;
  pc.minutes = tmp->tm_min;
  pc.seconds.tv_sec = tmp->tm_sec;
  pc.seconds.tv_nsec = Start_ns;
  tm.tm_isdst = tmp->tm_isdst;

  pc.meridian = MER24;
  pc.rel = RELATIVE_TIME_0;
  pc.timespec_seen = false;
  pc.rels_seen = false;
  pc.dates_seen = 0;
  pc.days_seen = 0;
  pc.times_seen = 0;
  pc.local_zones_seen = 0;
  pc.dsts_seen = 0;
  pc.zones_seen = 0;

#if HAVE_STRUCT_TM_TM_ZONE
  pc.local_time_zone_table[0].name = tmp->tm_zone;
  pc.local_time_zone_table[0].type = tLOCAL_ZONE;
  pc.local_time_zone_table[0].value = tmp->tm_isdst;
  pc.local_time_zone_table[1].name = NULL;

  /* Probe the names used in the next three calendar quarters, looking
     for a tm_isdst different from the one we already have.  */
  {
    int quarter;
    for (quarter = 1; quarter <= 3; quarter++)
      {
        time_t probe = Start + quarter * (90 * 24 * 60 * 60);
        struct tm const *probe_tm = localtime (&probe);
        if (probe_tm && probe_tm->tm_zone
            && probe_tm->tm_isdst != pc.local_time_zone_table[0].value)
          {
              {
                pc.local_time_zone_table[1].name = probe_tm->tm_zone;
                pc.local_time_zone_table[1].type = tLOCAL_ZONE;
                pc.local_time_zone_table[1].value = probe_tm->tm_isdst;
                pc.local_time_zone_table[2].name = NULL;
              }
            break;
          }
      }
  }
#else
#if HAVE_TZNAME
  {
# if !HAVE_DECL_TZNAME
    extern char *tzname[];
# endif
    int i;
    for (i = 0; i < 2; i++)
      {
        pc.local_time_zone_table[i].name = tzname[i];
        pc.local_time_zone_table[i].type = tLOCAL_ZONE;
        pc.local_time_zone_table[i].value = i;
      }
    pc.local_time_zone_table[i].name = NULL;
  }
#else
  pc.local_time_zone_table[0].name = NULL;
#endif
#endif

  if (pc.local_time_zone_table[0].name && pc.local_time_zone_table[1].name
      && ! strcmp (pc.local_time_zone_table[0].name,
                   pc.local_time_zone_table[1].name))
    {
      /* This locale uses the same abbreviation for standard and
         daylight times.  So if we see that abbreviation, we don't
         know whether it's daylight time.  */
      pc.local_time_zone_table[0].value = -1;
      pc.local_time_zone_table[1].name = NULL;
    }

  if (yyparse (&pc) != 0)
    goto fail;

  if (pc.timespec_seen)
    *result = pc.seconds;
  else
    {
      if (1 < (pc.times_seen | pc.dates_seen | pc.days_seen | pc.dsts_seen
               | (pc.local_zones_seen + pc.zones_seen)))
        goto fail;

      tm.tm_year = to_year (pc.year) - TM_YEAR_BASE;
      tm.tm_mon = pc.month - 1;
      tm.tm_mday = pc.day;
      if (pc.times_seen || (pc.rels_seen && ! pc.dates_seen && ! pc.days_seen))
        {
          tm.tm_hour = to_hour (pc.hour, pc.meridian);
          if (tm.tm_hour < 0)
            goto fail;
          tm.tm_min = pc.minutes;
          tm.tm_sec = pc.seconds.tv_sec;
        }
      else
        {
          tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
          pc.seconds.tv_nsec = 0;
        }

      /* Let mktime deduce tm_isdst if we have an absolute time stamp.  */
      if (pc.dates_seen | pc.days_seen | pc.times_seen)
        tm.tm_isdst = -1;

      /* But if the input explicitly specifies local time with or without
         DST, give mktime that information.  */
      if (pc.local_zones_seen)
        tm.tm_isdst = pc.local_isdst;

      tm0 = tm;

      Start = mktime (&tm);

      if (! mktime_ok (&tm0, &tm, Start))
        {
          if (! pc.zones_seen)
            goto fail;
          else
            {
              /* Guard against falsely reporting errors near the time_t
                 boundaries when parsing times in other time zones.  For
                 example, suppose the input string "1969-12-31 23:00:00 -0100",
                 the current time zone is 8 hours ahead of UTC, and the min
                 time_t value is 1970-01-01 00:00:00 UTC.  Then the min
                 localtime value is 1970-01-01 08:00:00, and mktime will
                 therefore fail on 1969-12-31 23:00:00.  To work around the
                 problem, set the time zone to 1 hour behind UTC temporarily
                 by setting TZ="XXX1:00" and try mktime again.  */

              long int time_zone = pc.time_zone;
              long int abs_time_zone = time_zone < 0 ? - time_zone : time_zone;
              long int abs_time_zone_hour = abs_time_zone / 60;
              int abs_time_zone_min = abs_time_zone % 60;
              char tz1buf[sizeof "XXX+0:00"
                          + sizeof pc.time_zone * CHAR_BIT / 3];
              if (!tz_was_altered)
                tz0 = get_tz (tz0buf);
              sprintf (tz1buf, "XXX%s%ld:%02d", &"-"[time_zone < 0],
                       abs_time_zone_hour, abs_time_zone_min);
              if (setenv ("TZ", tz1buf, 1) != 0)
                goto fail;
              tz_was_altered = true;
              tm = tm0;
              Start = mktime (&tm);
              if (! mktime_ok (&tm0, &tm, Start))
                goto fail;
            }
        }

      if (pc.days_seen && ! pc.dates_seen)
        {
          tm.tm_mday += ((pc.day_number - tm.tm_wday + 7) % 7
                         + 7 * (pc.day_ordinal
                                - (0 < pc.day_ordinal
                                   && tm.tm_wday != pc.day_number)));
          tm.tm_isdst = -1;
          Start = mktime (&tm);
          if (Start == (time_t) -1)
            goto fail;
        }

      /* Add relative date.  */
      if (pc.rel.year | pc.rel.month | pc.rel.day)
        {
          int year = tm.tm_year + pc.rel.year;
          int month = tm.tm_mon + pc.rel.month;
          int day = tm.tm_mday + pc.rel.day;
          if (((year < tm.tm_year) ^ (pc.rel.year < 0))
              | ((month < tm.tm_mon) ^ (pc.rel.month < 0))
              | ((day < tm.tm_mday) ^ (pc.rel.day < 0)))
            goto fail;
          tm.tm_year = year;
          tm.tm_mon = month;
          tm.tm_mday = day;
          tm.tm_hour = tm0.tm_hour;
          tm.tm_min = tm0.tm_min;
          tm.tm_sec = tm0.tm_sec;
          tm.tm_isdst = tm0.tm_isdst;
          Start = mktime (&tm);
          if (Start == (time_t) -1)
            goto fail;
        }

      /* The only "output" of this if-block is an updated Start value,
         so this block must follow others that clobber Start.  */
      if (pc.zones_seen)
        {
          long int delta = pc.time_zone * 60;
          time_t t1;
#ifdef HAVE_TM_GMTOFF
          delta -= tm.tm_gmtoff;
#else
          time_t t = Start;
          struct tm const *gmt = gmtime (&t);
          if (! gmt)
            goto fail;
          delta -= tm_diff (&tm, gmt);
#endif
          t1 = Start - delta;
          if ((Start < t1) != (delta < 0))
            goto fail;  /* time_t overflow */
          Start = t1;
        }

      /* Add relative hours, minutes, and seconds.  On hosts that support
         leap seconds, ignore the possibility of leap seconds; e.g.,
         "+ 10 minutes" adds 600 seconds, even if one of them is a
         leap second.  Typically this is not what the user wants, but it's
         too hard to do it the other way, because the time zone indicator
         must be applied before relative times, and if mktime is applied
         again the time zone will be lost.  */
      {
        long int sum_ns = pc.seconds.tv_nsec + pc.rel.ns;
        long int normalized_ns = (sum_ns % BILLION + BILLION) % BILLION;
        time_t t0 = Start;
        long int d1 = 60 * 60 * pc.rel.hour;
        time_t t1 = t0 + d1;
        long int d2 = 60 * pc.rel.minutes;
        time_t t2 = t1 + d2;
        intmax_t d3 = pc.rel.seconds;
        intmax_t t3 = t2 + d3;
        long int d4 = (sum_ns - normalized_ns) / BILLION;
        intmax_t t4 = t3 + d4;
        time_t t5 = t4;

        if ((d1 / (60 * 60) ^ pc.rel.hour)
            | (d2 / 60 ^ pc.rel.minutes)
            | ((t1 < t0) ^ (d1 < 0))
            | ((t2 < t1) ^ (d2 < 0))
            | ((t3 < t2) ^ (d3 < 0))
            | ((t4 < t3) ^ (d4 < 0))
            | (t5 != t4))
          goto fail;

        result->tv_sec = t5;
        result->tv_nsec = normalized_ns;
      }
    }

  goto done;

 fail:
  ok = false;
 done:
  if (tz_was_altered)
    ok &= (tz0 ? setenv ("TZ", tz0, 1) : unsetenv ("TZ")) == 0;
  if (tz0 != tz0buf)
    free (tz0);
  return ok;
}
