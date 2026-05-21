/*
 * libwinevbs minimal NLS provider.
 *
 * The fork does not ship/map locale.nls, so kernelbase's NLS_LOCALE_DATA tables
 * are never populated and the internal get_locale_info() accessor is dead. This
 * provider re-implements that single accessor against the host's user locale, so
 * every Win32 NLS consumer above it (GetLocaleInfoW, GetNumberFormatW,
 * GetCurrencyFormatW, GetDateFormatW, GetTimeFormatW and the variant/varformat
 * code built on them) keeps using Wine's own, tested formatting logic.
 *
 * Behaviour: the requested LCID is ignored; data always reflects the host user
 * locale, which is what a single-locale embedder (Visual Pinball) wants.
 */

#ifdef __LIBWINEVBS__

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "libwinevbs_nls.h"

/* Win32 GetLocaleInfoW return helpers, mirroring kernelbase/locale.c semantics. */

static int ret_str( const WCHAR *data, LCTYPE type, WCHAR *buffer, int len )
{
    int datalen = wcslen( data ) + 1;

    if (type & LOCALE_RETURN_NUMBER)
    {
        SetLastError( ERROR_INVALID_FLAGS );
        return 0;
    }
    if (!len) return datalen;
    if (datalen > len)
    {
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return 0;
    }
    memcpy( buffer, data, datalen * sizeof(WCHAR) );
    return datalen;
}

static int ret_num( UINT val, LCTYPE type, WCHAR *buffer, int len )
{
    WCHAR tmp[32];

    if (type & LOCALE_RETURN_NUMBER)
    {
        int ret = sizeof(UINT) / sizeof(WCHAR);
        if (!len) return ret;
        if (ret > len)
        {
            SetLastError( ERROR_INSUFFICIENT_BUFFER );
            return 0;
        }
        memcpy( buffer, &val, sizeof(val) );
        return ret;
    }

    switch (LOWORD(type))
    {
    case LOCALE_ILANGUAGE:
    case LOCALE_IDEFAULTLANGUAGE:
        swprintf( tmp, ARRAY_SIZE(tmp), L"%04x", val );
        break;
    default:
        swprintf( tmp, ARRAY_SIZE(tmp), L"%u", val );
        break;
    }
    return ret_str( tmp, type, buffer, len );
}


#ifdef _WIN32

/*
 * On native Windows the real OS already implements the full NLS API; the fork
 * just compiled a dead copy over it. Forward to the genuine kernel32 export,
 * substituting the user-default locale so behaviour follows the host machine.
 */

typedef int (WINAPI *get_locale_info_t)( LCID, LCTYPE, WCHAR *, int );

int libwinevbs_get_locale_info( LCID lcid, LCTYPE type, WCHAR *buffer, int len )
{
    static get_locale_info_t p_GetLocaleInfoW;

    if (!p_GetLocaleInfoW)
    {
        HMODULE k32 = GetModuleHandleW( L"kernel32.dll" );
        if (k32) p_GetLocaleInfoW = (get_locale_info_t)GetProcAddress( k32, "GetLocaleInfoW" );
    }
    if (!p_GetLocaleInfoW) return 0;

    /* Follow the host only for the neutral / default LCIDs; honour explicit ones
     * (en-US, de-DE, ...) so the OS returns exactly what was asked for. */
    if (PRIMARYLANGID( LANGIDFROMLCID(lcid) ) == LANG_NEUTRAL && lcid != LOCALE_INVARIANT)
        lcid = LOCALE_USER_DEFAULT;
    return p_GetLocaleInfoW( lcid, type, buffer, len );
}

#else /* POSIX */

#include <locale.h>
#include <langinfo.h>
#include <limits.h>

static locale_t host_locale(void)
{
    static locale_t loc;
    if (!loc) loc = newlocale( LC_ALL_MASK, "", (locale_t)0 );
    return loc;
}

/* An explicit English/invariant LCID means Wine wants its fixed canonical locale
 * (e.g. format_number_lcid() formats with LCID_EN_US to get a '.'-decimal string);
 * only the neutral / user-default LCIDs should follow the host machine. */
static int wants_invariant( LCID lcid )
{
    return lcid == LOCALE_INVARIANT || PRIMARYLANGID( LANGIDFROMLCID(lcid) ) == LANG_ENGLISH;
}

/* Convert a host-encoded C string (under the active thread locale) to WCHAR. */
static int to_wide( const char *s, WCHAR *dst, int cap )
{
    size_t n;

    if (!s) s = "";
    n = mbstowcs( (wchar_t *)dst, s, cap - 1 );
    if (n == (size_t)-1)
    {
        int i = 0;
        while (s[i] && i < cap - 1) { dst[i] = (unsigned char)s[i]; i++; }
        dst[i] = 0;
        return i;
    }
    dst[n] = 0;
    return (int)n;
}

/* POSIX grouping ("\3\3\0") -> Win32 SGROUPING ("3;0"). */
static void to_grouping( const char *g, WCHAR *out, int cap )
{
    int o = 0;

    if (!g || !*g)
    {
        wcsncpy( out, L"3;0", cap );
        out[cap - 1] = 0;
        return;
    }
    for (; *g && o < cap - 4; g++)
    {
        if (*g == CHAR_MAX)          /* no further grouping */
        {
            if (o > 0 && out[o - 1] == L';') o--;
            break;
        }
        o += swprintf( out + o, cap - o, L"%d;", (int)*g );
        if (!g[1]) { out[o++] = L'0'; break; }  /* last group repeats */
    }
    out[o] = 0;
}

static int date_order( const char *fmt )   /* 0=MDY 1=DMY 2=YMD */
{
    int pd = -1, pm = -1, py = -1, i;

    for (i = 0; fmt[i]; i++)
    {
        if (fmt[i] != '%') continue;
        switch (fmt[++i])
        {
        case 'd': case 'e': if (pd < 0) pd = i; break;
        case 'm':           if (pm < 0) pm = i; break;
        case 'y': case 'Y': if (py < 0) py = i; break;
        }
    }
    if (pm < 0 || pd < 0 || py < 0) return 0;
    if (py < pm && py < pd) return 2;
    if (pd < pm) return 1;
    return 0;
}

static WCHAR date_sep( const char *fmt )
{
    int i;
    for (i = 0; fmt[i]; i++)
    {
        if (fmt[i] == '%') { i++; continue; }
        if (fmt[i] == '/' || fmt[i] == '.' || fmt[i] == '-') return fmt[i];
    }
    return L'/';
}

static int is_24hour( const char *fmt )
{
    int i;
    for (i = 0; fmt[i]; i++)
        if (fmt[i] == '%' && (fmt[i + 1] == 'H' || fmt[i + 1] == 'k')) return 1;
    return 0;
}

/* Fixed en-US / invariant locale, matching Wine's canonical expectations. */
static int serve_invariant( LCTYPE type, WCHAR *buffer, int len )
{
    static const WCHAR *months[] = {
        L"January", L"February", L"March", L"April", L"May", L"June",
        L"July", L"August", L"September", L"October", L"November", L"December" };
    static const WCHAR *amonths[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    static const WCHAR *days[] = {  /* Win order: Monday..Sunday */
        L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday" };
    static const WCHAR *adays[] = {
        L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    LCTYPE lc = LOWORD( type );

    if (lc >= LOCALE_SMONTHNAME1 && lc <= LOCALE_SMONTHNAME1 + 11)
        return ret_str( months[lc - LOCALE_SMONTHNAME1], type, buffer, len );
    if (lc >= LOCALE_SABBREVMONTHNAME1 && lc <= LOCALE_SABBREVMONTHNAME1 + 11)
        return ret_str( amonths[lc - LOCALE_SABBREVMONTHNAME1], type, buffer, len );
    if (lc == LOCALE_SMONTHNAME13 || lc == LOCALE_SABBREVMONTHNAME13)
        return ret_str( L"", type, buffer, len );
    if (lc >= LOCALE_SDAYNAME1 && lc <= LOCALE_SDAYNAME7)
        return ret_str( days[lc - LOCALE_SDAYNAME1], type, buffer, len );
    if (lc >= LOCALE_SABBREVDAYNAME1 && lc <= LOCALE_SABBREVDAYNAME7)
        return ret_str( adays[lc - LOCALE_SABBREVDAYNAME1], type, buffer, len );

    switch (lc)
    {
    case LOCALE_SDECIMAL:        return ret_str( L".", type, buffer, len );
    case LOCALE_STHOUSAND:       return ret_str( L",", type, buffer, len );
    case LOCALE_SGROUPING:       return ret_str( L"3;0", type, buffer, len );
    case LOCALE_SNEGATIVESIGN:   return ret_str( L"-", type, buffer, len );
    case LOCALE_SPOSITIVESIGN:   return ret_str( L"", type, buffer, len );
    case LOCALE_IDIGITS:         return ret_num( 2, type, buffer, len );
    case LOCALE_ILZERO:          return ret_num( 1, type, buffer, len );
    case LOCALE_INEGNUMBER:      return ret_num( 1, type, buffer, len );
    case LOCALE_SCURRENCY:       return ret_str( L"$", type, buffer, len );
    case LOCALE_SMONDECIMALSEP:  return ret_str( L".", type, buffer, len );
    case LOCALE_SMONTHOUSANDSEP: return ret_str( L",", type, buffer, len );
    case LOCALE_SMONGROUPING:    return ret_str( L"3;0", type, buffer, len );
    case LOCALE_ICURRDIGITS:
    case LOCALE_IINTLCURRDIGITS: return ret_num( 2, type, buffer, len );
    case LOCALE_ICURRENCY:       return ret_num( 0, type, buffer, len );
    case LOCALE_INEGCURR:        return ret_num( 0, type, buffer, len );
    case LOCALE_IDATE:           return ret_num( 0, type, buffer, len );
    case LOCALE_SDATE:           return ret_str( L"/", type, buffer, len );
    case LOCALE_SSHORTDATE:      return ret_str( L"M/d/yyyy", type, buffer, len );
    case LOCALE_SLONGDATE:       return ret_str( L"dddd, MMMM d, yyyy", type, buffer, len );
    case LOCALE_SYEARMONTH:      return ret_str( L"MMMM yyyy", type, buffer, len );
    case LOCALE_SMONTHDAY:       return ret_str( L"MMMM d", type, buffer, len );
    case LOCALE_STIMEFORMAT:     return ret_str( L"h:mm:ss tt", type, buffer, len );
    case LOCALE_STIME:           return ret_str( L":", type, buffer, len );
    case LOCALE_ITIME:           return ret_num( 0, type, buffer, len );
    case LOCALE_ITLZERO:         return ret_num( 0, type, buffer, len );
    case LOCALE_S1159:           return ret_str( L"AM", type, buffer, len );
    case LOCALE_S2359:           return ret_str( L"PM", type, buffer, len );
    case LOCALE_IFIRSTDAYOFWEEK:  return ret_num( 0, type, buffer, len );
    case LOCALE_IFIRSTWEEKOFYEAR: return ret_num( 0, type, buffer, len );
    default:                     return ret_str( L"", type, buffer, len );
    }
}

int libwinevbs_get_locale_info( LCID lcid, LCTYPE type, WCHAR *buffer, int len )
{
    LCTYPE lc = LOWORD( type );
    locale_t saved;
    struct lconv *lv;
    WCHAR tmp[128];
    int ret;

    if (wants_invariant( lcid )) return serve_invariant( type, buffer, len );

    saved = uselocale( host_locale() );
    lv = localeconv();

    /* Month names: LOCALE_SMONTHNAME1..12 and the abbreviated range. */
    if (lc >= LOCALE_SMONTHNAME1 && lc <= LOCALE_SMONTHNAME1 + 11)
    {
        to_wide( nl_langinfo( MON_1 + (lc - LOCALE_SMONTHNAME1) ), tmp, ARRAY_SIZE(tmp) );
        ret = ret_str( tmp, type, buffer, len ); goto done;
    }
    if (lc >= LOCALE_SABBREVMONTHNAME1 && lc <= LOCALE_SABBREVMONTHNAME1 + 11)
    {
        to_wide( nl_langinfo( ABMON_1 + (lc - LOCALE_SABBREVMONTHNAME1) ), tmp, ARRAY_SIZE(tmp) );
        ret = ret_str( tmp, type, buffer, len ); goto done;
    }
    if (lc == LOCALE_SMONTHNAME13 || lc == LOCALE_SABBREVMONTHNAME13)
    {
        ret = ret_str( L"", type, buffer, len ); goto done;
    }
    /* Day names: Win SDAYNAME1=Monday, POSIX DAY_1=Sunday. */
    if (lc >= LOCALE_SDAYNAME1 && lc <= LOCALE_SDAYNAME7)
    {
        to_wide( nl_langinfo( DAY_1 + ((lc - LOCALE_SDAYNAME1 + 1) % 7) ), tmp, ARRAY_SIZE(tmp) );
        ret = ret_str( tmp, type, buffer, len ); goto done;
    }
    if (lc >= LOCALE_SABBREVDAYNAME1 && lc <= LOCALE_SABBREVDAYNAME7)
    {
        to_wide( nl_langinfo( ABDAY_1 + ((lc - LOCALE_SABBREVDAYNAME1 + 1) % 7) ), tmp, ARRAY_SIZE(tmp) );
        ret = ret_str( tmp, type, buffer, len ); goto done;
    }

    switch (lc)
    {
    /* numbers */
    case LOCALE_SDECIMAL:        to_wide( lv->decimal_point, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"." );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_STHOUSAND:       to_wide( lv->thousands_sep, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"," );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SGROUPING:       to_grouping( lv->grouping, tmp, ARRAY_SIZE(tmp) );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SNEGATIVESIGN:   to_wide( lv->negative_sign, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"-" );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SPOSITIVESIGN:   to_wide( lv->positive_sign, tmp, ARRAY_SIZE(tmp) );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_IDIGITS:         ret = ret_num( 2, type, buffer, len ); break;
    case LOCALE_ILZERO:          ret = ret_num( 1, type, buffer, len ); break;
    case LOCALE_INEGNUMBER:      ret = ret_num( 1, type, buffer, len ); break;

    /* currency */
    case LOCALE_SCURRENCY:       to_wide( lv->currency_symbol, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"$" );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SMONDECIMALSEP:  to_wide( lv->mon_decimal_point, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"." );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SMONTHOUSANDSEP: to_wide( lv->mon_thousands_sep, tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"," );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SMONGROUPING:    to_grouping( lv->mon_grouping, tmp, ARRAY_SIZE(tmp) );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_ICURRDIGITS:
    case LOCALE_IINTLCURRDIGITS: ret = ret_num( lv->frac_digits == CHAR_MAX ? 2 : lv->frac_digits,
                                                type, buffer, len ); break;
    case LOCALE_ICURRENCY:       /* 0:$1  1:1$  2:$ 1  3:1 $ */
                                 ret = ret_num( (lv->p_cs_precedes ? 0 : 1) + (lv->p_sep_by_space ? 2 : 0),
                                                type, buffer, len ); break;
    case LOCALE_INEGCURR:        ret = ret_num( lv->n_cs_precedes ? 0 : 1, type, buffer, len ); break;

    /* date */
    case LOCALE_IDATE:           ret = ret_num( date_order( nl_langinfo( D_FMT ) ), type, buffer, len ); break;
    case LOCALE_SDATE:           tmp[0] = date_sep( nl_langinfo( D_FMT ) ); tmp[1] = 0;
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SSHORTDATE:
    {
        WCHAR sep = date_sep( nl_langinfo( D_FMT ) );
        switch (date_order( nl_langinfo( D_FMT ) ))
        {
        case 1:  swprintf( tmp, ARRAY_SIZE(tmp), L"d%lcM%lcyyyy", sep, sep ); break;
        case 2:  swprintf( tmp, ARRAY_SIZE(tmp), L"yyyy%lcM%lcd", sep, sep ); break;
        default: swprintf( tmp, ARRAY_SIZE(tmp), L"M%lcd%lcyyyy", sep, sep ); break;
        }
        ret = ret_str( tmp, type, buffer, len ); break;
    }
    case LOCALE_SLONGDATE:
        switch (date_order( nl_langinfo( D_FMT ) ))
        {
        case 1:  wcscpy( tmp, L"dddd, d MMMM yyyy" ); break;
        case 2:  wcscpy( tmp, L"yyyy MMMM d, dddd" ); break;
        default: wcscpy( tmp, L"dddd, MMMM d, yyyy" ); break;
        }
        ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_SYEARMONTH:      ret = ret_str( L"MMMM yyyy", type, buffer, len ); break;
    case LOCALE_SMONTHDAY:       ret = ret_str( L"MMMM d", type, buffer, len ); break;

    /* time */
    case LOCALE_STIMEFORMAT:
        ret = ret_str( is_24hour( nl_langinfo( T_FMT ) ) ? L"HH:mm:ss" : L"h:mm:ss tt",
                       type, buffer, len ); break;
    case LOCALE_STIME:           ret = ret_str( L":", type, buffer, len ); break;
    case LOCALE_ITIME:           ret = ret_num( is_24hour( nl_langinfo( T_FMT ) ), type, buffer, len ); break;
    case LOCALE_ITLZERO:         ret = ret_num( 0, type, buffer, len ); break;
    case LOCALE_S1159:           to_wide( nl_langinfo( AM_STR ), tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"AM" );
                                 ret = ret_str( tmp, type, buffer, len ); break;
    case LOCALE_S2359:           to_wide( nl_langinfo( PM_STR ), tmp, ARRAY_SIZE(tmp) );
                                 if (!tmp[0]) wcscpy( tmp, L"PM" );
                                 ret = ret_str( tmp, type, buffer, len ); break;

    /* calendar / week */
    case LOCALE_IFIRSTDAYOFWEEK:  ret = ret_num( 0, type, buffer, len ); break;  /* Monday */
    case LOCALE_IFIRSTWEEKOFYEAR: ret = ret_num( 0, type, buffer, len ); break;

    default:
        /* Unmapped: hand back a benign empty string so callers don't error out. */
        ret = ret_str( L"", type, buffer, len );
        break;
    }

done:
    uselocale( saved );
    return ret;
}

#endif /* POSIX */

#endif /* __LIBWINEVBS__ */
