/*
 * libwinevbs minimal NLS provider.
 *
 * Serves locale data for the host's user locale so that Wine's own
 * GetLocaleInfoW / GetNumberFormatW / GetCurrencyFormatW / GetDateFormatW /
 * GetTimeFormatW implementations work without the locale.nls data file.
 */

#ifndef __LIBWINEVBS_NLS_H
#define __LIBWINEVBS_NLS_H

#ifdef __LIBWINEVBS__

#include "windef.h"
#include "winnls.h"

extern int libwinevbs_get_locale_info( LCID lcid, LCTYPE type, WCHAR *buffer, int len );

#endif /* __LIBWINEVBS__ */

#endif /* __LIBWINEVBS_NLS_H */
